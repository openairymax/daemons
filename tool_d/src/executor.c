// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file executor.c
 * @brief Tool-executor implementation (production-grade process management).
 * @details Real tool execution based on popen/pclose, supporting timeout,
 *          output capture and error handling.
 */

/* P3.18 (ACC-DT27): sandbox public API (airy_sandbox_t, permission_type_t,
 * airy_sandbox_create_default, airy_sandbox_invoke, etc.) */
#include "airy_sandbox.h"

#include "syscalls.h"
#include "daemon_errors.h"
#include "daemon_security.h"
#include "executor.h"
#include "builtin.h"
#include "daemon_platform_ext.h"
#include "safety_guard_bridge.h"
#include "svc_logger.h"
#include "tool_approval.h"
#include "hall_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cjson/cJSON.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

/* ---------- Improvement 1 (P1d): parallel-tool concurrency gating
 * (writer-preferring RwLock) ----------
 *
 * Semantics: READ tools hold the read gate and run concurrently; WRITE
 * tools hold the write gate and run mutually exclusive/serially.
 * writer-preferring: waiting writers block new readers, so write tools
 * are never starved by read tools. exec->lock only guards the stats
 * fields (no whole-section locking, avoiding serializing all tools). */

typedef struct {
    airy_mtx_t lock;
    airy_cond_t cond;
    int readers;
    int writer;
    int writer_waiting;
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

    tool_rw_gate_t rw_gate;

    tool_approval_ctx_t *approval_ctx;
    safety_guard_bridge_t *safety_bridge;
    /* P3.18 (ACC-DT27): tool execution sandbox — a mandatory security layer
     * (not an optional enhancement). Together with approval_ctx it forms a
     * two-tier fail-closed security architecture:
     *   - approval_ctx: policy approval based on tool metadata and params
     *   - sandbox: permission/quota/audit interception based on syscall number
     * When sandbox is NULL (init failed), tool_executor_run refuses to execute
     * any tool. */
    airy_sandbox_t *sandbox;
    /* P0: tool-level interactive permission approval (Claude Code style
     * permission prompt). Enabled at creation when AIRY_TOOL_APPROVAL_MODE is
     * "interactive". When static approval denies, if this is enabled the
     * request is queued as pending and blocks waiting for a tool.approve
     * decision. */
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
    /* P0: create the interactive approval manager (reads
     * AIRY_TOOL_APPROVAL_MODE to decide whether to enable). Creation failure
     * only disables interactive approval; it does not block normal executor
     * creation nor static fail-closed approval. */
    exec->interactive = interactive_approval_create();

    /* P3.18 (ACC-DT27): initialize the tool execution sandbox.
     *
     * Design notes:
     * - sandbox is a mandatory security layer (not an optional enhancement),
     *   unlike approval_ctx. NULL approval_ctx denies fail-closed; NULL
     *   sandbox likewise denies fail-closed.
     * - On creation failure exec->sandbox stays NULL and tool_executor_run
     *   refuses to execute any tool.
     * - airy_sandbox_manager_init is idempotent; repeated calls are safe
     *   (process-level singleton).
     * - Explicitly add the PERM_ALLOW SYS_TOOL_EXECUTE rule to make the intent
     *   clear (allowed by default anyway), for auditability and forward
     *   compatibility with future default-policy changes. */
    airy_err_t sb_init = airy_sandbox_manager_init();
    if (sb_init != AIRY_SUCCESS) {
        SVC_LOG_WARN("C-L08: sandbox_manager_init failed (rc=%d) — tools will be fail-closed",
                     (int)sb_init);
    } else {
        airy_err_t sb_create = airy_sandbox_create_default("tool_d", "tool_d", &exec->sandbox);
        if (sb_create != AIRY_SUCCESS || !exec->sandbox) {
            SVC_LOG_ERROR(
                "C-L08: sandbox_create_default failed (rc=%d) — tools will be fail-closed",
                (int)sb_create);
            exec->sandbox = NULL;
        } else {
            airy_err_t sb_rule =
                airy_sandbox_add_rule(exec->sandbox, SYS_TOOL_EXECUTE, PERM_ALLOW, NULL);
            if (sb_rule != AIRY_SUCCESS) {
                SVC_LOG_WARN("C-L08: sandbox_add_rule(SYS_TOOL_EXECUTE, ALLOW) failed (rc=%d)",
                             (int)sb_rule);
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
    /* P3.17: the executor owns approval_ctx (tool_executor_set_approval_ctx
     * transfers ownership). safety_bridge is created in set_approval_ctx and
     * is also owned by the executor. */
    if (exec->approval_ctx) {
        tool_approval_destroy(exec->approval_ctx);
        exec->approval_ctx = NULL;
    }
    if (exec->safety_bridge) {
        safety_guard_bridge_destroy(exec->safety_bridge);
        exec->safety_bridge = NULL;
    }
    /* P3.18 (ACC-DT27): destroy the sandbox. Note: airy_sandbox_manager_destroy
     * is NOT called because the manager is a process-level singleton possibly
     * shared by other executors; its lifecycle is managed by process exit or
     * explicit cleanup. */
    if (exec->sandbox) {
        airy_sandbox_destroy(exec->sandbox);
        exec->sandbox = NULL;
    }

    if (exec->interactive) {
        interactive_approval_destroy(exec->interactive);
        exec->interactive = NULL;
    }
    tool_rw_gate_destroy(&exec->rw_gate);
    airy_mtx_destroy(&exec->lock);
    AIRY_FREE(exec);
}

void tool_executor_set_approval_ctx(tool_executor_t *exec, tool_approval_ctx_t *approval_ctx)
{
    if (!exec)
        return;
    airy_mtx_lock(&exec->lock);
    exec->approval_ctx = approval_ctx;
    airy_mtx_unlock(&exec->lock);
    if (approval_ctx) {
        SVC_LOG_INFO("C-L05: Approval context attached to executor");

        if (!exec->safety_bridge) {
            safety_guard_bridge_config_t bridge_cfg;
            __builtin_memset(&bridge_cfg, 0, sizeof(bridge_cfg));
            bridge_cfg.enable_permission_guard = true;
            bridge_cfg.enable_rate_limit_guard = true;
            bridge_cfg.enable_content_filter = true;
            bridge_cfg.enable_input_sanitization = true;
            bridge_cfg.enable_resource_quota = true;
            bridge_cfg.enable_audit_guard = true;
            bridge_cfg.rate_limit_per_minute = 0;
            bridge_cfg.max_params_size = 0;
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

        tool_approval_set_safety_guard_bridge(approval_ctx, exec->safety_bridge);
    }
}

/* 执行链事件（2.8b）：工具执行结果 → result 事件。best-effort，写失败
 * 绝不影响工具结果返回。task 分组键取调用 agent（无则工具 id）。
 * 内置工具（builtin:xxx）与外部 execvp 两条路径共用，保证所有真实执行
 * 都进入事件流单一真相源。 */
static void tool_hall_emit_result(const tool_metadata_t *meta, const char *caller_agent,
                                  const tool_result_t *result)
{
    const char *evt_task =
        (caller_agent && caller_agent[0]) ? caller_agent :
                                            (meta->id && meta->id[0] ? meta->id : "tools");
    cJSON *evt = cJSON_CreateObject();
    if (!evt)
        return;
    cJSON_AddStringToObject(evt, "event", "tool_result");
    cJSON_AddStringToObject(evt, "tool", meta->id ? meta->id : "");
    cJSON_AddStringToObject(evt, "name", meta->name ? meta->name : "");
    cJSON_AddNumberToObject(evt, "success", result->success ? 1 : 0);
    cJSON_AddNumberToObject(evt, "exit_code", (double)result->exit_code);
    cJSON_AddNumberToObject(evt, "duration_ms", (double)result->duration_ms);
    if (result->error && result->error[0])
        cJSON_AddStringToObject(evt, "error", result->error);
    char *s = cJSON_PrintUnformatted(evt);
    if (s) {
        (void)daemon_hall_write(evt_task, "result", meta->id, s);
        cJSON_free(s);
    }
    cJSON_Delete(evt);
}

/* P1-2 工具调用级日志（可观测性）：每次工具执行输出一条结构化调用记录，
 * 含调用方 agent / 工具名 / 参数摘要 / 耗时 / 结果状态。用于 CLI 与日志
 * 分析定位"任务执行链路"（哪个 agent 调了什么工具、成功与否、耗时）。 */
static void tool_call_log(const tool_metadata_t *meta, const char *caller_agent,
                          const char *params_json, const tool_result_t *result)
{
    char params[192];
    size_t plen = params_json ? strlen(params_json) : 0;
    if (plen >= sizeof(params)) {
        __builtin_memcpy(params, params_json, sizeof(params) - 5);
        __builtin_memcpy(params + sizeof(params) - 5, "...", 4);
        params[sizeof(params) - 1] = '\0';
    } else if (plen > 0) {
        __builtin_memcpy(params, params_json, plen + 1);
    } else {
        params[0] = '\0';
    }
    /* 参数 JSON 压缩为单行，避免换行破坏日志记录结构 */
    for (char *q = params; *q; q++)
        if (*q == '\n' || *q == '\r')
            *q = ' ';

    const char *st = "?";
    if (result) {
        switch (result->failure_class) {
        case TOOL_RESULT_CLASS_SUCCESS:
            st = result->success ? "success" : "fail";
            break;
        case TOOL_RESULT_CLASS_NORMAL_FAIL:
            st = "fail";
            break;
        case TOOL_RESULT_CLASS_RESPOND_TO_MODEL:
            st = "respond";
            break;
        default:
            st = "fatal";
            break;
        }
    }
    const char *err = (result && result->error && result->error[0]) ? result->error : NULL;
    SVC_LOG_INFO("[tool] call agent=%s tool=%s dur=%ums status=%s exit=%d params=%s%s%s",
                 caller_agent ? caller_agent : "?", meta->name ? meta->name : "?",
                 result ? result->duration_ms : 0u, st, result ? result->exit_code : -1,
                 params, err ? " err=" : "", err ? err : "");
}

int tool_executor_run(tool_executor_t *exec, const tool_metadata_t *meta, const char *params_json,
                      const char *agent_id, tool_result_t **out_result)
{
    if (!exec || !meta || !out_result) {
        SVC_LOG_ERROR("tool_executor_run: NULL parameter (exec/meta/out_result)");
        return AIRY_ERR_INVALID_PARAM;
    }

    const char *caller_agent = (agent_id && agent_id[0]) ? agent_id : NULL;

    *out_result = NULL;

    tool_result_t *result = (tool_result_t *)AIRY_CALLOC(1, sizeof(tool_result_t));
    if (!result) {
        SVC_LOG_ERROR("tool_executor_run: calloc failed for result");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    /* Improvement 1 (P1d): concurrency gating — READ tools run concurrently
     * under a read gate, WRITE tools serialize under a write gate.
     * exec->lock only protects the stats fields (no longer held for the whole
     * section, avoiding serializing all tools). */
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
        result->failure_class = TOOL_RESULT_CLASS_RESPOND_TO_MODEL;
        result->duration_ms = 0;
        *out_result = result;
        tool_rw_gate_unlock(&exec->rw_gate);
        return AIRY_ERR_INVALID_PARAM;
    }

    /* BAN-211/235: execute directly via execvp (no shell), no SEC-011 shell
     * metacharacter check needed. params_json is passed as a single argv
     * element to the tool, which parses the JSON itself. */

    /* ── C-L05: Cupolas SafetyGuard -> tool_d tool approval ──
     * P3.17 (ACC-DT18) fail-closed: refuse execution when approval_ctx is NULL.
     * Legacy code `if (exec->approval_ctx)` skipped approval and executed when
     * the ctx was not set, equivalent to the security system being disabled —
     * violating the zero-debt security principle.
     * Fix: ctx unset = security system not configured = refuse execution
     * (fail-closed). service.c injects a default approval_ctx
     * (enable_approval=true) right after creating the executor. */
    if (!exec->approval_ctx) {
        SVC_LOG_ERROR("C-L05: approval_ctx is NULL — tool execution DENIED (fail-closed). "
                      "Call tool_executor_set_approval_ctx() before executing tools.");
        result->success = 0;
        result->output = AIRY_STRDUP("");
        result->error = AIRY_STRDUP("Safety approval system not configured (approval_ctx is NULL)");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_FATAL;
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
            /* P0: tool-level interactive permission approval.
             * When enabled (AIRY_TOOL_APPROVAL_MODE=interactive), a static
             * approval denial no longer fails closed directly; instead the
             * request is queued as pending and blocks waiting for a
             * tool.approve decision:
             *   - allow    -> let this execution through
             *   - always   -> allow and add a persistent ACL rule
             *   - deny/timeout -> return EPERM (error contains "User denied
             *     tool execution") */
            if (exec->interactive && interactive_approval_is_enabled(exec->interactive)) {

                const char *agent = caller_agent;
                if (!agent) {
                    agent = tool_approval_get_agent_id(exec->approval_ctx);
                }
                /* During interactive blocking, exec->lock is not held (P1d:
                 * the lock only protects stats), but concurrency gating is
                 * retained (a pending write-tool approval blocks other tools,
                 * which is semantically correct). */
                airy_approval_outcome_t outcome = AIRY_APPROVAL_DENIED;
                char *request_id =
                    interactive_approval_block(exec->interactive, meta->name ? meta->name : "?",
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
                    /* Add a persistent ACL rule (agent_id + tool name +
                     * allow) so subsequent identical calls pass static
                     * approval directly. */
                    if (agent && meta->name) {
                        int ar = daemon_security_add_acl_rule(agent, meta->name, true);
                        if (ar != 0) {
                            SVC_LOG_WARN("C-L05: add_acl_rule('%s','%s') failed rc=%d", agent,
                                         meta->name, ar);
                        }
                    }
                } else {

                    SVC_LOG_ERROR("C-L05: Tool '%s' denied by user (interactive) or timed out",
                                  meta->name ? meta->name : "?");
                    result->success = 0;
                    result->output = AIRY_STRDUP("");
                    result->error = AIRY_STRDUP("User denied tool execution");
                    result->exit_code = -1;
                    result->failure_class = TOOL_RESULT_CLASS_RESPOND_TO_MODEL;
                    result->duration_ms = 0;
                    *out_result = result;
                    tool_rw_gate_unlock(&exec->rw_gate);
                    return AIRY_EPERM;
                }
            } else {
                SVC_LOG_ERROR("C-L05: Tool approval denied for '%s': %s",
                              meta->name ? meta->name : "?", approval_detail.reason);
                result->success = 0;
                result->output = AIRY_STRDUP("");
                result->error = AIRY_STRDUP(approval_detail.reason[0] ?
                                                approval_detail.reason :
                                                "Tool execution denied by safety guard");
                result->exit_code = -1;
                result->failure_class = TOOL_RESULT_CLASS_RESPOND_TO_MODEL;
                result->duration_ms = 0;
                *out_result = result;
                tool_rw_gate_unlock(&exec->rw_gate);
                return AIRY_EPERM;
            }
        }
        SVC_LOG_INFO("C-L05: Tool '%s' approved (decision=%d)", meta->name ? meta->name : "?",
                     (int)approval_detail.decision);
    }

    /* Builtin tools (builtin:xxx): real implementations dispatch directly
     * (fs_read/fs_write/fs_list/shell_run), already passed approval above
     * (fail-closed ACL), no external execvp process needed. */
    if (tool_builtin_is_builtin(meta->executable)) {
        int brc = tool_builtin_run(meta->id, params_json, result);
        result->duration_ms = (uint32_t)((time(NULL) - start_time) * 1000);
        if (brc == 0 && result->success) {
            result->failure_class = TOOL_RESULT_CLASS_SUCCESS;
            airy_mtx_lock(&exec->lock);
            exec->success_count++;
            airy_mtx_unlock(&exec->lock);
        } else {

            result->failure_class = TOOL_RESULT_CLASS_NORMAL_FAIL;
        }
        tool_call_log(meta, caller_agent, params_json, result);
        tool_hall_emit_result(meta, caller_agent, result);
        *out_result = result;
        tool_rw_gate_unlock(&exec->rw_gate);
        return brc;
    }

    /* BAN-211/235: build argv and execute directly via execvp (no shell),
     * eliminating command-injection risk. params_json is passed as a single
     * argv element; the tool parses it itself. */
    const char *argv[3];
    int arg_count = 0;
    argv[arg_count++] = meta->executable;
    if (params_json && strlen(params_json) > 0) {
        argv[arg_count++] = params_json;
    }
    argv[arg_count] = NULL;

    size_t cap_size = 1024 * 1024;
    char *output_buffer = (char *)AIRY_MALLOC(cap_size);
    if (!output_buffer) {
        SVC_LOG_ERROR("tool_executor_run: malloc failed for output buffer (size=%zu)", cap_size);
        result->success = 0;
        result->output = AIRY_STRDUP("");
        result->error = AIRY_STRDUP("Memory allocation failed for output buffer");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_FATAL;
        result->duration_ms = (uint32_t)((time(NULL) - start_time) * 1000);
        *out_result = result;
        tool_rw_gate_unlock(&exec->rw_gate);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    output_buffer[0] = '\0';

    uint32_t timeout_ms = (uint32_t)exec->manager.timeout_sec * 1000;

    /* P3.18 (ACC-DT27): execute the tool through the sandbox — three layers of
     * permission/quota/audit interception.
     *
     * Two-tier fail-closed security architecture:
     * 1. SafetyGuard approval (above): policy approval based on tool metadata
     *    and params
     * 2. Sandbox interception (below): permission/quota/audit based on syscall
     *    number
     * If either layer denies, the tool does not run. NULL sandbox (init
     * failed) also denies.
     *
     * Execution path: airy_sandbox_invoke -> permission_check -> quota_check ->
     * airy_syscall_invoke -> sys_tool_execute -> airy_process_run_capture
     */
    if (!exec->sandbox) {
        SVC_LOG_ERROR("C-L08: sandbox is NULL — tool execution DENIED (fail-closed). "
                      "Sandbox initialization failed during executor creation.");
        result->success = 0;
        result->output = AIRY_STRDUP("");
        result->error = AIRY_STRDUP("Sandbox not configured (initialization failed)");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_FATAL;
        result->duration_ms = (uint32_t)((time(NULL) - start_time) * 1000);
        AIRY_FREE(output_buffer);
        *out_result = result;
        tool_rw_gate_unlock(&exec->rw_gate);
        return AIRY_EPERM;
    }

    tool_execute_args_t targs = {.executable = meta->executable,
                                 .argv = (char *const *)argv,
                                 .timeout_ms = timeout_ms,
                                 .output_buffer = output_buffer,
                                 .cap_size = cap_size,
                                 .exec_result = 0};
    void *invoke_args[1] = {&targs};
    void *sb_out_result = NULL;
    airy_err_t sb_ret =
        airy_sandbox_invoke(exec->sandbox, SYS_TOOL_EXECUTE, invoke_args, 1, &sb_out_result);
    if (sb_ret != AIRY_SUCCESS) {
        SVC_LOG_ERROR("C-L08: sandbox denied tool '%s' execution (rc=%d) — fail-closed",
                      meta->name ? meta->name : "?", (int)sb_ret);
        result->success = 0;
        result->output = AIRY_STRDUP("");
        result->error = AIRY_STRDUP("Tool execution denied by sandbox (permission/quota/state)");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_FATAL;
        result->duration_ms = (uint32_t)((time(NULL) - start_time) * 1000);
        AIRY_FREE(output_buffer);
        *out_result = result;
        tool_rw_gate_unlock(&exec->rw_gate);
        return AIRY_EPERM;
    }

    /* sandbox_invoke succeeded: targs.exec_result holds the
     * airy_process_run_capture return value (0-255=exit code; -1=start
     * failure; -2=timeout) */
    int ret = targs.exec_result;
    (void)sb_out_result;

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
        result->failure_class = TOOL_RESULT_CLASS_RESPOND_TO_MODEL;
    } else if (ret == -2) {
        SVC_LOG_ERROR("tool_executor_run: command timed out after %u ms (executable=%s)",
                      timeout_ms, meta->executable ? meta->executable : "NULL");
        result->success = 0;
        result->error = AIRY_STRDUP("Command timed out");
        result->exit_code = -1;
        result->failure_class = TOOL_RESULT_CLASS_NORMAL_FAIL;
    } else if (ret == 0) {
        result->success = 1;
        result->exit_code = 0;
        result->failure_class = TOOL_RESULT_CLASS_SUCCESS;
        /* P2: keep the counter update under exec->lock (as the builtin branch
         * above does) - it was racing with concurrent tool invocations. */
        airy_mtx_lock(&exec->lock);
        exec->success_count++;
        airy_mtx_unlock(&exec->lock);
    } else if (ret > 0) {
        SVC_LOG_ERROR("tool_executor_run: command failed with exit code %d (executable=%s)", ret,
                      meta->executable ? meta->executable : "NULL");
        result->success = 0;
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Command exited with code %d", ret);
        result->error = AIRY_STRDUP(err_msg);
        result->exit_code = ret;
        result->failure_class = TOOL_RESULT_CLASS_NORMAL_FAIL;
    } else {

        SVC_LOG_ERROR("tool_executor_run: command killed by signal %d (executable=%s)", -ret,
                      meta->executable ? meta->executable : "NULL");
        result->success = 0;
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Command killed by signal %d", -ret);
        result->error = AIRY_STRDUP(err_msg);
        result->exit_code = ret;
        result->failure_class = TOOL_RESULT_CLASS_NORMAL_FAIL;
    }

    result->duration_ms = (uint32_t)((time(NULL) - start_time) * 1000);

    tool_call_log(meta, caller_agent, params_json, result);
    tool_hall_emit_result(meta, caller_agent, result);

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
