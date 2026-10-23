// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tool_approval.c
 * @brief Cupolas SafetyGuard -> tool_d tool-approval adapter impl.
 */

#include "tool_approval.h"
#include "safety_guard_bridge.h"
#include "daemon_errors.h"
#include "daemon_security.h"
#include "error.h"
#include "logger.h"
#include "airy_memory.h"
#include "string_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

struct tool_approval_ctx {
    tool_approval_config_t config;
    char agent_id[128];
    /* 执行面 worker 池并发后审批检查可多线程同时进入，
     * 统计计数器改为原子量（仅统计用途，relaxed 序即可）。 */
    _Atomic uint64_t total_checks;
    _Atomic uint64_t denied_count;
    _Atomic uint64_t sanitized_count;

    safety_guard_bridge_t *bridge;
};

tool_approval_ctx_t *tool_approval_create(const tool_approval_config_t *cfg)
{
    tool_approval_ctx_t *ctx = (tool_approval_ctx_t *)AIRY_CALLOC(1, sizeof(tool_approval_ctx_t));
    if (!ctx) {
        AIRY_LOG_ERROR("tool_approval_create: alloc failed");
        return NULL;
    }

    if (cfg) {
        ctx->config = *cfg;
        if (cfg->agent_id) {
            snprintf(ctx->agent_id, sizeof(ctx->agent_id), "%s", cfg->agent_id);
        }
    } else {
        ctx->config.agent_id = ctx->agent_id;
        ctx->config.enable_safety_guard_chain = false;
        ctx->config.enable_audit_logging = true;
        ctx->config.permission_rules = NULL;
        snprintf(ctx->agent_id, sizeof(ctx->agent_id), "unknown");
    }

    AIRY_LOG_INFO("Tool approval context created (safety_guard=%d, audit=%d)",
                  ctx->config.enable_safety_guard_chain, ctx->config.enable_audit_logging);
    return ctx;
}

void tool_approval_destroy(tool_approval_ctx_t *ctx)
{
    if (!ctx) return;
    AIRY_LOG_INFO("Tool approval destroyed (checks=%llu denied=%llu sanitized=%llu)",
                  (unsigned long long)atomic_load_explicit(&ctx->total_checks,
                                                           memory_order_relaxed),
                  (unsigned long long)atomic_load_explicit(&ctx->denied_count,
                                                           memory_order_relaxed),
                  (unsigned long long)atomic_load_explicit(&ctx->sanitized_count,
                                                           memory_order_relaxed));
    AIRY_FREE(ctx);
}

void tool_approval_set_safety_guard_bridge(tool_approval_ctx_t *ctx, safety_guard_bridge_t *bridge)
{
    if (!ctx) return;
    ctx->bridge = bridge;
    if (bridge) {
        AIRY_LOG_INFO("SafetyGuard bridge attached to approval context");
    } else {
        AIRY_LOG_INFO("SafetyGuard bridge detached from approval context");
    }
}

static int tool_approval_sanitize_params(tool_approval_ctx_t *ctx, const char *tool_name,
                                         const char *params_json, char *sanitized_params,
                                         size_t sanitized_size)
{
    if (!ctx || !tool_name || !params_json || !sanitized_params || sanitized_size == 0) {
        return AIRY_ERR_INVALID_PARAM;
    }

    char sanitized_tool[256];
    int ret = daemon_sanitize_tool_params(tool_name, params_json, sanitized_tool,
                                          sizeof(sanitized_tool), sanitized_params, sanitized_size);

    if (ret == 0) {

        if (strcmp(params_json, sanitized_params) != 0) {
            AIRY_LOG_INFO("Tool params sanitized for '%s'", tool_name);
            atomic_fetch_add_explicit(&ctx->sanitized_count, 1, memory_order_relaxed);
        }
    } else {
        AIRY_LOG_WARN("Tool param sanitization failed for '%s': ret=%d", tool_name, ret);
    }

    return ret;
}

/* detail 初始化：默认拒绝（fail-closed），所有通过位清零。 */
static void ap_detail_init(tool_approval_detail_t *detail)
{
    if (!detail) return;
    AIRY_MEMSET(detail, 0, sizeof(*detail));
    detail->decision = TOOL_APPROVAL_DENIED;
}

/* 清洗结果回填：ok 为调用方的「确已改写」判定，空串不覆盖已填内容。 */
static void ap_take_san(tool_approval_detail_t *detail, int ok, const char *san)
{
    if (detail && ok && san[0])
        snprintf(detail->sanitized_params, sizeof(detail->sanitized_params), "%s", san);
}

/* 拒绝出口：计数 + 可选审计，两条拒绝路径共用同一语义。 */
static int ap_deny(tool_approval_ctx_t *ctx, const char *agent_id, const char *tool_name)
{
    atomic_fetch_add_explicit(&ctx->denied_count, 1, memory_order_relaxed);
    if (ctx->config.enable_audit_logging)
        daemon_audit_log_event("tool_d", "tool_execute_denied", tool_name, 0, agent_id);
    return AIRY_ERR_PERMISSION_DENIED;
}

/* SafetyGuard 引导路径的清洗预检：发生改写时置位并抄录，返回是否改写。 */
static int ap_sanitize(tool_approval_ctx_t *ctx, const char *tool_name, const char *params_json,
                       tool_approval_detail_t *detail)
{
    if (!params_json || !params_json[0]) return 0;
    char sanitized[4096] = {0};
    int ret = tool_approval_sanitize_params(ctx, tool_name, params_json, sanitized,
                                            sizeof(sanitized));
    if (ret != 0 || strcmp(params_json, sanitized) == 0) return 0;
    if (detail) {
        detail->params_were_sanitized = 1;
        ap_take_san(detail, 1, sanitized);
    }
    return 1;
}

/* SafetyGuard 拒绝分支的 detail 回填。 */
static void ap_bridge_deny(tool_approval_detail_t *detail,
                           const safety_guard_bridge_result_t *br)
{
    if (!detail) return;
    detail->permission_check_passed = br->permission_passed;
    detail->safety_guard_passed = 0;
    detail->params_were_sanitized = br->input_sanitized;
    snprintf(detail->reason, sizeof(detail->reason), "%s",
             br->denial_reason[0] ? br->denial_reason : "Denied by SafetyGuard chain");
    ap_take_san(detail, br->input_sanitized, br->sanitized_params);
}

/* SafetyGuard 放行分支的 detail 回填。 */
static void ap_bridge_allow(tool_approval_detail_t *detail,
                            const safety_guard_bridge_result_t *br, const char *tool_name)
{
    if (!detail) return;
    detail->decision = br->input_sanitized ? TOOL_APPROVAL_SANITIZED : TOOL_APPROVAL_ALLOWED;
    detail->permission_check_passed = br->permission_passed;
    detail->safety_guard_passed = 1;
    detail->params_were_sanitized = br->input_sanitized;
    ap_take_san(detail, br->input_sanitized, br->sanitized_params);
    snprintf(detail->reason, sizeof(detail->reason),
             "Approved by SafetyGuard chain (%d/%d guards) for tool '%s'",
             br->guards_executed, br->guard_chain_length, tool_name);
}

/* 策略链（SafetyGuard）审批出口。 */
static int ap_bridge(tool_approval_ctx_t *ctx, const char *agent_id,
                     const tool_metadata_t *meta, const char *params_json,
                     const char *tool_name, tool_approval_detail_t *detail)
{
    safety_guard_bridge_result_t br;
    int ret =
        safety_guard_bridge_check_for_agent(ctx->bridge, agent_id, meta, params_json, &br);
    if (ret != 0) {
        AIRY_LOG_WARN("SafetyGuard bridge denied '%s' for '%s': %s", tool_name, agent_id,
                      br.denial_reason);
        ap_bridge_deny(detail, &br);
        return ap_deny(ctx, agent_id, tool_name);
    }
    ap_bridge_allow(detail, &br, tool_name);
    AIRY_LOG_INFO("SafetyGuard bridge approved '%s' "
                  "(%d/%d guards executed)",
                  tool_name, br.guards_executed, br.guard_chain_length);
    return 0;
}

/* 机制链（本地清洗 + Cupolas ACL）审批出口。 */
static int ap_local(tool_approval_ctx_t *ctx, const char *agent_id, const char *tool_name,
                    const char *params_json, tool_approval_detail_t *detail)
{
    (void)ap_sanitize(ctx, tool_name, params_json, detail);

    /* ── Step 2: permission check (Cupolas) ──
     * Return-code inversion fix:
     * daemon_check_tool_permission returns 0=allowed, non-zero=denied
     * (fail-closed). Legacy code `if (!perm_ret)` entered the deny path when
     * perm_ret==0 (allowed) and let perm_ret<0 (denied) through — the logic
     * was fully inverted, causing:
     *   - tools with no ACL entry to be wrongly allowed (should deny
     *     fail-closed)
     *   - ACL-authorized tools to be wrongly denied
     * Fixed to `if (perm_ret != 0)`, consistent with safety_guard_bridge.c
     * L218. */
    int perm_ret = daemon_check_tool_permission(agent_id, tool_name, "execute");
    if (perm_ret != 0) {
        AIRY_LOG_WARN("Permission denied for agent='%s' tool='%s' (perm_ret=%d)", agent_id,
                      tool_name, perm_ret);
        if (detail) {
            detail->permission_check_passed = 0;
            snprintf(detail->reason, sizeof(detail->reason),
                     "Permission denied: agent '%s' cannot execute tool '%s'", agent_id,
                     tool_name);
        }
        return ap_deny(ctx, agent_id, tool_name);
    }

    if (detail) {
        detail->permission_check_passed = 1;
        if (ctx->config.enable_safety_guard_chain)
            detail->safety_guard_passed = 1;
    }
    if (ctx->config.enable_audit_logging)
        daemon_audit_log_event("tool_d", "tool_execute", tool_name, 1, agent_id);
    if (detail) {
        detail->decision = detail->params_were_sanitized ? TOOL_APPROVAL_SANITIZED
                                                         : TOOL_APPROVAL_ALLOWED;
        snprintf(detail->reason, sizeof(detail->reason),
                 detail->params_were_sanitized
                     ? "Approved with sanitized params for tool '%s'"
                     : "Approved for tool '%s'",
                 tool_name);
    }
    return 0;
}

/* 审批主体显式参数化。旧实现通过临时改写共享
 * ctx->config.agent_id 再恢复来支持按主体审批，前提是 tool_d 单线程；
 * 执行面 worker 池并发后该前提失效，会发生审批主体串写（agent A 的
 * 请求以 agent B 的身份通过审批）与数据竞态。改为参数传递后
 * ctx->config.agent_id 创建后不再被写，并发安全。 */
static int approval_check_as(tool_approval_ctx_t *ctx, const char *subject,
                             const tool_metadata_t *meta, const char *params_json,
                             tool_approval_detail_t *detail)
{
    ap_detail_init(detail);
    atomic_fetch_add_explicit(&ctx->total_checks, 1, memory_order_relaxed);
    const char *tool_name = meta->name ? meta->name : "unknown";
    if (ctx->bridge) return ap_bridge(ctx, subject, meta, params_json, tool_name, detail);
    return ap_local(ctx, subject, tool_name, params_json, detail);
}

int tool_approval_check(tool_approval_ctx_t *ctx, const tool_metadata_t *meta,
                        const char *params_json, tool_approval_detail_t *detail)
{
    if (!ctx || !meta) {
        return AIRY_ERR_INVALID_PARAM;
    }
    const char *subject = ctx->config.agent_id ? ctx->config.agent_id : "unknown";
    return approval_check_as(ctx, subject, meta, params_json, detail);
}

const char *tool_approval_get_agent_id(const tool_approval_ctx_t *ctx)
{
    if (!ctx)
        return NULL;
    return ctx->agent_id;
}

int tool_approval_check_for_agent(tool_approval_ctx_t *ctx, const char *agent_id,
                                  const tool_metadata_t *meta, const char *params_json,
                                  tool_approval_detail_t *detail)
{
    if (!ctx || !meta) {
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!agent_id || !agent_id[0]) {
        return tool_approval_check(ctx, meta, params_json, detail);
    }

    /* 主体经参数直传（见 approval_check_as 注释），
     * 不再临时改写共享 ctx->config.agent_id，多 worker 并发安全。 */
    return approval_check_as(ctx, agent_id, meta, params_json, detail);
}
