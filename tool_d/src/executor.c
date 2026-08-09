// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#include "airy_memory.h"
#include "error.h"
/**
 * @file executor.c
 * @brief 工具执行器实现（生产级进程管理）
 * @details 基于popen/pclose的真实工具执行，支持超时、输出捕获、错误处理
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

/* P3.18 (ACC-DT27): sandbox 公共 API（airy_sandbox_t, permission_type_t,
 * airy_sandbox_create_default, airy_sandbox_invoke 等） */
#include "airy_sandbox.h"
/* P3.18 (ACC-DT27): SYS_TOOL_EXECUTE syscall 号 + tool_execute_args_t 结构体 */
#include "syscalls.h"
#include "daemon_errors.h"
#include "daemon_security.h"
#include "executor.h"
#include "builtin.h"
#include "daemon_platform_ext.h"
#include "safety_guard_bridge.h"
#include "svc_logger.h"
#include "tool_approval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <sys/wait.h> /* POSIX 专用头文件（当前文件未直接使用，保留以备扩展） */
#endif

/* ---------- 改进1（P1d）：并行工具并发门控（writer-preferring RwLock） ----------
 *
 * 语义：READ 工具持读门并发执行；WRITE 工具持写门互斥串行。
 * writer-preferring：等待的写者阻塞新读者，保证写工具不被读工具饿死。
 * exec->lock 仅保护统计字段（不再全段持锁，避免串行化所有工具）。 */

typedef struct {
    airy_mtx_t lock;
    airy_cond_t cond;
    int readers;        /* 活跃读门持有者数 */
    int writer;         /* 活跃写门持有者（0/1） */
    int writer_waiting; /* 等待中的写者数（写优先防饿死） */
} tool_rw_gate_t;

static void tool_rw_gate_init(tool_rw_gate_t *g)
{
    airy_mtx_init(&g->lock);
    airy_cond_init(&g->cond);
    g->readers = 0;
    g->writer = 0;
    g->writer_waiting = 0;
}

static void tool_rw_gate_destroy(tool_rw_gate_t *g)
{
    airy_cond_destroy(&g->cond);
    airy_mtx_destroy(&g->lock);
}

static void tool_rw_gate_rdlock(tool_rw_gate_t *g)
{
    airy_mtx_lock(&g->lock);
    while (g->writer || g->writer_waiting > 0)
        airy_cond_wait(&g->cond, &g->lock);
    g->readers++;
    airy_mtx_unlock(&g->lock);
}

static void tool_rw_gate_wrlock(tool_rw_gate_t *g)
{
    airy_mtx_lock(&g->lock);
    g->writer_waiting++;
    while (g->writer || g->readers > 0)
        airy_cond_wait(&g->cond, &g->lock);
    g->writer_waiting--;
    g->writer = 1;
    airy_mtx_unlock(&g->lock);
}

static void tool_rw_gate_unlock(tool_rw_gate_t *g)
{
    airy_mtx_lock(&g->lock);
    if (g->writer) {
        g->writer = 0;
        airy_cond_broadcast(&g->cond);
    } else if (g->readers > 0) {
        g->readers--;
        if (g->readers == 0)
            airy_cond_broadcast(&g->cond);
    }
    airy_mtx_unlock(&g->lock);
}

struct tool_executor {
    tool_executor_config_t manager;
    airy_mtx_t lock;
    uint64_t total_executions;
    uint64_t success_count;
    /* P1d：并行工具并发门控（READ 并发 / WRITE 串行） */
    tool_rw_gate_t rw_gate;
    /* C-L05: Cupolas SafetyGuard → tool_d 工具审批 */
    tool_approval_ctx_t *approval_ctx;
    safety_guard_bridge_t *safety_bridge;
    /* P3.18 (ACC-DT27): 工具执行沙箱 — 强制安全层（非可选增强）。
     * 与 approval_ctx 形成"审批 + 拦截"双层 fail-closed 安全架构：
     *   - approval_ctx: 基于工具元数据和参数的策略审批
     *   - sandbox: 基于 syscall 号的权限/配额/审计拦截
     * sandbox 为 NULL（初始化失败）时 tool_executor_run 拒绝执行任何工具。 */
    airy_sandbox_t *sandbox;
    /* P0: 工具级交互式权限审批（Claude Code 风格 permission prompt）。
     * 创建时读取 AIRY_TOOL_APPROVAL_MODE，为"interactive"时启用。
     * 静态审批拒绝时，若此处启用则入队 pending 并阻塞等待 tool.approve 决议。 */
    interactive_approval_t *interactive;
};

tool_executor_t *tool_executor_create(const tool_executor_config_t *cfg)
{
    tool_executor_config_t local_cfg;
    if (!cfg) {
        __builtin_memset(&local_cfg, 0, sizeof(local_cfg));
        local_cfg.max_workers = 1;
        local_cfg.timeout_sec = 30;
        cfg = &local_cfg;
    }

    tool_executor_t *exec = (tool_executor_t *)AIRY_CALLOC(1, sizeof(tool_executor_t));
    if (!exec) {
        SVC_LOG_ERROR("tool_executor_create: calloc failed for executor");
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    exec->manager = *cfg;
    if (exec->manager.timeout_sec == 0) {
        exec->manager.timeout_sec = 30;
    }
    if (airy_mtx_init(&exec->lock) != 0) {
        SVC_LOG_ERROR("tool_executor_create: mutex init failed");
        AIRY_FREE(exec);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    tool_rw_gate_init(&exec->rw_gate);
    exec->total_executions = 0;
    exec->success_count = 0;
    exec->approval_ctx = NULL;
    exec->safety_bridge = NULL;
    exec->sandbox = NULL;
    /* P0: 创建交互式审批管理器（读取 AIRY_TOOL_APPROVAL_MODE 决定是否启用）。
     * 创建失败仅禁用交互审批，不阻断 executor 正常创建与静态 fail-closed 审批。 */
    exec->interactive = interactive_approval_create();

    /* P3.18 (ACC-DT27): 初始化工具执行沙箱。
     *
     * 设计说明：
     * - sandbox 是强制安全层（非可选增强），与 approval_ctx 不同。
     *   approval_ctx 为 NULL 时 fail-closed 拒绝；sandbox 为 NULL 时同样 fail-closed。
     * - 创建失败时 exec->sandbox 保持 NULL，tool_executor_run 会拒绝执行任何工具。
     * - airy_sandbox_manager_init 幂等，重复调用安全（进程级单例）。
     * - 显式添加 PERM_ALLOW SYS_TOOL_EXECUTE 规则，使意图明确（虽默认即允许），
     *   便于审计和未来默认策略变更时的前向兼容。 */
    airy_err_t sb_init = airy_sandbox_manager_init();
    if (sb_init != AIRY_SUCCESS) {
        SVC_LOG_WARN("C-L08: sandbox_manager_init failed (rc=%d) — tools will be fail-closed",
                     (int)sb_init);
    } else {
        airy_err_t sb_create =
            airy_sandbox_create_default("tool_d", "tool_d", &exec->sandbox);
        if (sb_create != AIRY_SUCCESS || !exec->sandbox) {
            SVC_LOG_ERROR("C-L08: sandbox_create_default failed (rc=%d) — tools will be fail-closed",
                          (int)sb_create);
            exec->sandbox = NULL;
        } else {
            airy_err_t sb_rule = airy_sandbox_add_rule(
                exec->sandbox, SYS_TOOL_EXECUTE, PERM_ALLOW, NULL);
            if (sb_rule != AIRY_SUCCESS) {
                SVC_LOG_WARN("C-L08: sandbox_add_rule(SYS_TOOL_EXECUTE, ALLOW) failed (rc=%d)",
                             (int)sb_rule);
                /* 规则添加失败不致命：默认策略是 PERM_ALLOW，工具仍可执行 */
            }
            SVC_LOG_INFO("C-L08: Sandbox initialized for tool executor (allow SYS_TOOL_EXECUTE)");
        }
    }

    return exec;
}

tool_executor_t *tool_executor_create_ex(const tool_executor_config_t *ecfg)
{
    return tool_executor_create(ecfg);
}

void tool_executor_destroy(tool_executor_t *exec)
{
    if (!exec)
        return;
    SVC_LOG_INFO("Executor destroyed: total=%llu, success=%llu",
                 (unsigned long long)exec->total_executions,
                 (unsigned long long)exec->success_count);
    /* P3.17: executor 拥有 approval_ctx（tool_executor_set_approval_ctx 转移所有权）。
     * safety_bridge 在 set_approval_ctx 中创建，也由 executor 拥有。*/
    if (exec->approval_ctx) {
        tool_approval_destroy(exec->approval_ctx);
        exec->approval_ctx = NULL;
    }
    if (exec->safety_bridge) {
        safety_guard_bridge_destroy(exec->safety_bridge);
        exec->safety_bridge = NULL;
    }
    /* P3.18 (ACC-DT27): 销毁沙箱。注意：不调用 airy_sandbox_manager_destroy，
     * 因为管理器是进程级单例，可能被其他 executor 共享。管理器生命周期由
     * 进程退出或显式清理管理。 */
    if (exec->sandbox) {
        airy_sandbox_destroy(exec->sandbox);
        exec->sandbox = NULL;
    }
    /* P0: 释放交互式审批管理器 */
    if (exec->interactive) {
        interactive_approval_destroy(exec->interactive);
        exec->interactive = NULL;
    }
    tool_rw_gate_destroy(&exec->rw_gate);
    airy_mtx_destroy(&exec->lock);
    AIRY_FREE(exec);
}

/* C-L05: 设置工具审批上下文 */
void tool_executor_set_approval_ctx(tool_executor_t *exec, tool_approval_ctx_t *approval_ctx)
{
    if (!exec)
        return;
    airy_mtx_lock(&exec->lock);
    exec->approval_ctx = approval_ctx;
    airy_mtx_unlock(&exec->lock);
    if (approval_ctx) {
        SVC_LOG_INFO("C-L05: Approval context attached to executor");

        /* C-L05: 创建 SafetyGuard 桥接层并注入到审批上下文 */
        if (!exec->safety_bridge) {
            safety_guard_bridge_config_t bridge_cfg;
            __builtin_memset(&bridge_cfg, 0, sizeof(bridge_cfg));
            bridge_cfg.enable_permission_guard = true;
            bridge_cfg.enable_rate_limit_guard = true;
            bridge_cfg.enable_content_filter = true;
            bridge_cfg.enable_input_sanitization = true;
            bridge_cfg.enable_resource_quota = true;
            bridge_cfg.enable_audit_guard = true;
            bridge_cfg.rate_limit_per_minute = 0;  /* 无限制 */
            bridge_cfg.max_params_size = 0;        /* 无限制 */
            bridge_cfg.denied_patterns = NULL;
            bridge_cfg.agent_id = "tool_d";

            exec->safety_bridge = safety_guard_bridge_create(&bridge_cfg);
            if (exec->safety_bridge) {
                SVC_LOG_INFO("C-L05: SafetyGuard bridge created for executor");
            } else {
                SVC_LOG_WARN("C-L05: Failed to create SafetyGuard bridge, "
                             "falling back to local checks");
            }
        }

        /* 将桥接层注入到审批上下文 */
        tool_approval_set_safety_guard_bridge(approval_ctx, exec->safety_bridge);
    }
}

int tool_executor_run(tool_executor_t *exec, const tool_metadata_t *meta, const char *params_json,
                      const char *agent_id, tool_result_t **out_result)
{
    if (!exec || !meta || !out_result) {
        SVC_LOG_ERROR("tool_executor_run: NULL parameter (exec/meta/out_result)");
        return AIRY_ERR_INVALID_PARAM;
    }

    /* 审批主体：未显式传入时回退审批上下文默认（"tool_d"） */
    const char *caller_agent = (agent_id && agent_id[0]) ? agent_id : NULL;

    *out_result = NULL;

    tool_result_t *result = (tool_result_t *)AIRY_CALLOC(1, sizeof(tool_result_t));
    if (!result) {
        SVC_LOG_ERROR("tool_executor_run: calloc failed for result");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    /* 改进1（P1d）：并发门控 — READ 工具读门并发执行，WRITE 工具写门互斥串行。
     * exec->lock 仅保护统计字段（不再全段持锁，避免串行化所有工具）。 */
    if (meta->access == TOOL_ACCESS_READ)
        tool_rw_gate_rdlock(&exec->rw_gate);
    else
        tool_rw_gate_wrlock(&exec->rw_gate);

    airy_mtx_lock(&exec->lock);
    exec->total_executions++;
    airy_mtx_unlock(&exec->lock);
    time_t start_time = time(NULL);

    if (!meta->executable || strlen(meta->executable) == 0) {
        SVC_LOG_ERROR("tool_executor_run: no executable specified in tool metadata (id=%s)",
                      meta->id ? meta->id : "?");
        result->success = 0;
        result->output = AIRY_STRDUP("");
        result->error = AIRY_STRDUP("No executable specified in tool metadata");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_RESPOND_TO_MODEL; /* 配置缺陷：回传上层修正 */
        result->duration_ms = 0;
        *out_result = result;
        tool_rw_gate_unlock(&exec->rw_gate);
        return AIRY_ERR_INVALID_PARAM;
    }

    /* BAN-211/235: 使用 execvp 直接执行（不经 shell），无需 SEC-011 shell 元字符检查。
     * params_json 作为单个 argv 元素传递给工具，工具自行解析 JSON。 */

    /* ── C-L05: Cupolas SafetyGuard → tool_d 工具审批 ──
     * P3.17 (ACC-DT18) fail-closed：approval_ctx 为 NULL 时拒绝执行。
     * 历史代码 `if (exec->approval_ctx)` 在 ctx 未设置时跳过审批直接执行，
     * 等同于安全系统未启用——违反零债务安全原则。
     * 修正：ctx 未设置 = 安全系统未配置 = 拒绝执行（fail-closed）。
     * service.c 在创建 executor 后立即注入默认 approval_ctx（enable_approval=true）。*/
    if (!exec->approval_ctx) {
        SVC_LOG_ERROR("C-L05: approval_ctx is NULL — tool execution DENIED (fail-closed). "
                      "Call tool_executor_set_approval_ctx() before executing tools.");
        result->success = 0;
        result->output = AIRY_STRDUP("");
        result->error = AIRY_STRDUP("Safety approval system not configured (approval_ctx is NULL)");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_FATAL; /* 安全系统缺失：fail-closed 致命 */
        result->duration_ms = 0;
        *out_result = result;
        tool_rw_gate_unlock(&exec->rw_gate);
        return AIRY_EPERM;
    }

    {
        tool_approval_detail_t approval_detail;
        int app_ret = tool_approval_check_for_agent(exec->approval_ctx, caller_agent, meta,
                                                    params_json, &approval_detail);
        if (app_ret != 0) {
            /* P0: 工具级交互式权限审批。
             * 启用时（AIRY_TOOL_APPROVAL_MODE=interactive）被静态审批拒绝不再直接
             * fail-closed，而是入队 pending 并阻塞等待 tool.approve 决议：
             *   - allow    → 放行本次执行
             *   - always   → 放行并加入持久 ACL 规则
             *   - deny/超时 → 返回 EPERM（错误信息含 "User denied tool execution"） */
            if (exec->interactive && interactive_approval_is_enabled(exec->interactive)) {
                /* 审批主体：真实调用方 agent（若透传），否则回退上下文默认 */
                const char *agent = caller_agent;
                if (!agent) {
                    agent = tool_approval_get_agent_id(exec->approval_ctx);
                }
                /* 交互阻塞期间不持有 exec->lock（P1d：锁仅保护统计字段），
                 * 但保留并发门控（write 工具审批未决时阻塞其他工具，语义正确）。 */
                airy_approval_outcome_t outcome = AIRY_APPROVAL_DENIED;
                char *request_id = interactive_approval_block(
                    exec->interactive, meta->name ? meta->name : "?",
                    agent ? agent : "unknown", params_json, &outcome);
                if (request_id) {
                    AIRY_FREE(request_id);
                }

                if (outcome == AIRY_APPROVAL_ALLOWED) {
                    SVC_LOG_INFO("C-L05: Tool '%s' approved by user (interactive, one-shot)",
                                 meta->name ? meta->name : "?");
                } else if (outcome == AIRY_APPROVAL_ALWAYS) {
                    SVC_LOG_INFO("C-L05: Tool '%s' approved by user (interactive, always)",
                                 meta->name ? meta->name : "?");
                    /* 加入持久 ACL 规则（agent_id + 工具名 + allow），
                     * 使后续同类调用直接通过静态审批。 */
                    if (agent && meta->name) {
                        int ar = daemon_security_add_acl_rule(agent, meta->name, true);
                        if (ar != 0) {
                            SVC_LOG_WARN("C-L05: add_acl_rule('%s','%s') failed rc=%d",
                                         agent, meta->name, ar);
                        }
                    }
                } else {
                    /* deny 决议或超时：返回 EPERM */
                    SVC_LOG_ERROR("C-L05: Tool '%s' denied by user (interactive) or timed out",
                                  meta->name ? meta->name : "?");
                    result->success = 0;
                    result->output = AIRY_STRDUP("");
                    result->error = AIRY_STRDUP("User denied tool execution");
                    result->exit_code = -1;
                    result->failure_class = TOOL_RESULT_CLASS_RESPOND_TO_MODEL; /* 审批拒绝：回传上层改道 */
                    result->duration_ms = 0;
                    *out_result = result;
                    tool_rw_gate_unlock(&exec->rw_gate);
                    return AIRY_EPERM;
                }
            } else {
                SVC_LOG_ERROR(
                    "C-L05: Tool approval denied for '%s': %s",
                    meta->name ? meta->name : "?", approval_detail.reason);
                result->success = 0;
                result->output = AIRY_STRDUP("");
                result->error = AIRY_STRDUP(approval_detail.reason[0]
                                                   ? approval_detail.reason
                                                   : "Tool execution denied by safety guard");
                result->exit_code = -1;
                result->failure_class = TOOL_RESULT_CLASS_RESPOND_TO_MODEL; /* 审批拒绝：回传上层 */
                result->duration_ms = 0;
                *out_result = result;
                tool_rw_gate_unlock(&exec->rw_gate);
                return AIRY_EPERM;
            }
        }
        SVC_LOG_INFO("C-L05: Tool '%s' approved (decision=%d)", meta->name ? meta->name : "?",
                     (int)approval_detail.decision);
    }

    /* 内置工具（builtin:xxx）：真实实现直接分派（fs_read/fs_write/fs_list/shell_run），
     * 已通过上方 approval（fail-closed ACL），无需 execvp 外部进程。 */
    if (tool_builtin_is_builtin(meta->executable)) {
        int brc = tool_builtin_run(meta->id, params_json, result);
        result->duration_ms = (uint32_t)((time(NULL) - start_time) * 1000);
        if (brc == 0 && result->success) {
            result->failure_class = TOOL_RESULT_CLASS_SUCCESS;
            airy_mtx_lock(&exec->lock);
            exec->success_count++;
            airy_mtx_unlock(&exec->lock);
        } else {
            /* 内建工具失败（含工具自身返回错误）：普通失败，回传继续 */
            result->failure_class = TOOL_RESULT_CLASS_NORMAL_FAIL;
        }
        *out_result = result;
        tool_rw_gate_unlock(&exec->rw_gate);
        return brc;
    }

    /* BAN-211/235: 构建 argv 并通过 execvp 直接执行（不经 shell），消除命令注入风险。
     * params_json 作为单个 argv 元素传递给工具，工具自行解析。 */
    const char *argv[3];
    int arg_count = 0;
    argv[arg_count++] = meta->executable;
    if (params_json && strlen(params_json) > 0) {
        argv[arg_count++] = params_json;
    }
    argv[arg_count] = NULL;

    /* 分配输出捕获缓冲区（1MB 上限，覆盖绝大多数工具输出，同时防止失控输出 OOM） */
    size_t cap_size = 1024 * 1024;
    char *output_buffer = (char *)AIRY_MALLOC(cap_size);
    if (!output_buffer) {
        SVC_LOG_ERROR("tool_executor_run: malloc failed for output buffer (size=%zu)", cap_size);
        result->success = 0;
        result->output = AIRY_STRDUP("");
        result->error = AIRY_STRDUP("Memory allocation failed for output buffer");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_FATAL; /* 内存分配失败：系统级致命 */
        result->duration_ms = (uint32_t)((time(NULL) - start_time) * 1000);
        *out_result = result;
        tool_rw_gate_unlock(&exec->rw_gate);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    output_buffer[0] = '\0';

    uint32_t timeout_ms = (uint32_t)exec->manager.timeout_sec * 1000;

    /* P3.18 (ACC-DT27): 经 sandbox 执行工具 — permission/quota/audit 三层拦截。
     *
     * 双层 fail-closed 安全架构：
     * 1. SafetyGuard 审批（上方 L189-209）：基于工具元数据和参数的策略审批
     * 2. Sandbox 拦截（下方）：基于 syscall 号的权限/配额/审计拦截
     * 任一层拒绝则工具不执行。sandbox 为 NULL（初始化失败）同样拒绝。
     *
     * 执行路径：airy_sandbox_invoke → permission_check → quota_check →
     * airy_syscall_invoke → sys_tool_execute → airy_process_run_capture
     */
    if (!exec->sandbox) {
        SVC_LOG_ERROR("C-L08: sandbox is NULL — tool execution DENIED (fail-closed). "
                      "Sandbox initialization failed during executor creation.");
        result->success = 0;
        result->output = AIRY_STRDUP("");
        result->error = AIRY_STRDUP("Sandbox not configured (initialization failed)");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_FATAL; /* 安全沙箱缺失：fail-closed 致命 */
        result->duration_ms = (uint32_t)((time(NULL) - start_time) * 1000);
        AIRY_FREE(output_buffer);
        *out_result = result;
        tool_rw_gate_unlock(&exec->rw_gate);
        return AIRY_EPERM;
    }

    tool_execute_args_t targs = {
        .executable = meta->executable,
        .argv = (char *const *)argv,
        .timeout_ms = timeout_ms,
        .output_buffer = output_buffer,
        .cap_size = cap_size,
        .exec_result = 0
    };
    void *invoke_args[1] = { &targs };
    void *sb_out_result = NULL;
    airy_err_t sb_ret = airy_sandbox_invoke(exec->sandbox, SYS_TOOL_EXECUTE,
                                                     invoke_args, 1, &sb_out_result);
    if (sb_ret != AIRY_SUCCESS) {
        SVC_LOG_ERROR("C-L08: sandbox denied tool '%s' execution (rc=%d) — fail-closed",
                      meta->name ? meta->name : "?", (int)sb_ret);
        result->success = 0;
        result->output = AIRY_STRDUP("");
        result->error = AIRY_STRDUP("Tool execution denied by sandbox (permission/quota/state)");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_FATAL; /* 安全层拦截：fail-closed 致命 */
        result->duration_ms = (uint32_t)((time(NULL) - start_time) * 1000);
        AIRY_FREE(output_buffer);
        *out_result = result;
        tool_rw_gate_unlock(&exec->rw_gate);
        return AIRY_EPERM;
    }

    /* sandbox_invoke 成功：targs.exec_result 含 airy_process_run_capture 返回值
     * (0-255=exit code; -1=启动失败; -2=超时) */
    int ret = targs.exec_result;
    (void)sb_out_result;  /* sys_tool_execute 返回 SUCCESS/EFAIL，已由 sb_ret 覆盖检查 */

    /* 缩减缓冲区到实际输出长度，避免内存浪费 */
    size_t actual_len = strlen(output_buffer);
    if (actual_len + 1 < cap_size) {
        char *shrunk = (char *)AIRY_REALLOC(output_buffer, actual_len + 1);
        if (shrunk) {
            output_buffer = shrunk;
        }
    }

    result->output = output_buffer;
    result->error = NULL;

    if (ret == -1) {
        SVC_LOG_ERROR("tool_executor_run: failed to start command '%s'",
                      meta->executable ? meta->executable : "NULL");
        result->success = 0;
        result->error = AIRY_STRDUP("Failed to execute command: execvp failed");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_RESPOND_TO_MODEL; /* 启动失败：回传上层继续 */
    } else if (ret == -2) {
        SVC_LOG_ERROR("tool_executor_run: command timed out after %u ms (executable=%s)",
                      timeout_ms, meta->executable ? meta->executable : "NULL");
        result->success = 0;
        result->error = AIRY_STRDUP("Command timed out");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_NORMAL_FAIL; /* 超时：普通失败（transient 可重试） */
    } else if (ret == 0) {
        result->success = 1;
        result->exit_code = 0;
        result->failure_class = TOOL_RESULT_CLASS_SUCCESS;
        exec->success_count++;
    } else if (ret > 0) {
        SVC_LOG_ERROR("tool_executor_run: command failed with exit code %d (executable=%s)", ret,
                      meta->executable ? meta->executable : "NULL");
        result->success = 0;
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Command exited with code %d", ret);
        result->error = AIRY_STRDUP(err_msg);
        result->exit_code = ret;
        result->failure_class = TOOL_RESULT_CLASS_NORMAL_FAIL; /* 退出码>0：普通失败 */
    } else {
        /* ret < 0 且非 -1/-2：信号终止（exit_code = -signum） */
        SVC_LOG_ERROR("tool_executor_run: command killed by signal %d (executable=%s)", -ret,
                      meta->executable ? meta->executable : "NULL");
        result->success = 0;
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Command killed by signal %d", -ret);
        result->error = AIRY_STRDUP(err_msg);
        result->exit_code = ret;
        result->failure_class = TOOL_RESULT_CLASS_NORMAL_FAIL; /* 信号终止：普通失败 */
    }

    result->duration_ms = (uint32_t)((time(NULL) - start_time) * 1000);

    *out_result = result;
    tool_rw_gate_unlock(&exec->rw_gate);
    return AIRY_OK;
}

int tool_executor_run_async(tool_executor_t *exec, const tool_metadata_t *meta,
                            const char *params_json, const char *agent_id,
                            tool_execute_callback_t callback, void *user_data,
                            tool_result_t **out_result)
{
    if (!exec || !meta) {
        SVC_LOG_ERROR("tool_executor_run_async: NULL parameter (exec/meta)");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (out_result) {
        *out_result = NULL;
    }

    tool_result_t *result = NULL;
    int ret = tool_executor_run(exec, meta, params_json, agent_id, &result);

    if (callback && result) {
        callback(result, user_data);
    }

    if (out_result) {
        *out_result = result;
    }

    return ret;
}

/* ---------- P0 交互式审批：service 层调用 ---------- */

bool tool_executor_interactive_enabled(tool_executor_t *exec)
{
    if (!exec || !exec->interactive) {
        return false;
    }
    return interactive_approval_is_enabled(exec->interactive);
}

char *tool_executor_interactive_pending_list(tool_executor_t *exec)
{
    if (!exec || !exec->interactive) {
        return NULL;
    }
    return interactive_approval_pending_list_json(exec->interactive);
}

int tool_executor_interactive_resolve(tool_executor_t *exec, const char *request_id,
                                      const char *decision)
{
    if (!exec || !exec->interactive) {
        return AIRY_ERR_NOT_FOUND;
    }
    return interactive_approval_resolve(exec->interactive, request_id, decision);
}
