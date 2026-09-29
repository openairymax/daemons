// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file gate.c
 * @brief 审批门实现：静态 ACL、SafetyGuard 链与交互式审批的唯一归口。
 *
 * gate 拥有 approval_ctx / safety_bridge / interactive 三件套；core/executor
 * 仅经 core/approval_gate.h 的窄接口使用本机制（0.1.18 §12.16.4）。
 */

#include "core/approval_gate.h"
#include "tool_approval.h"
#include "safety_guard_bridge.h"
#include "tool_interactive_approval.h"

#include "daemon_security.h"
#include "svc_logger.h"
#include "airy_memory.h"
#include "error.h"

#include <stdlib.h>

struct approval_gate {
    tool_approval_ctx_t *ctx;
    safety_guard_bridge_t *bridge;
    interactive_approval_t *interactive;
};

/* SafetyGuard 桥（C-L05）：六 guard 全开的默认装配，桥创建失败降级为
 * 本地静态检查（不阻断，静态审批仍 fail-closed）。 */
static void gate_attach_bridge(approval_gate_t *gate)
{
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

    gate->bridge = safety_guard_bridge_create(&bridge_cfg);
    if (gate->bridge) {
        SVC_LOG_INFO("SafetyGuard bridge created for approval gate");
    } else {
        SVC_LOG_WARN("Failed to create SafetyGuard bridge, falling back to local checks");
    }
    tool_approval_set_safety_guard_bridge(gate->ctx, gate->bridge);
}

approval_gate_t *tool_approval_gate_create(const tool_approval_config_t *cfg)
{
    if (!cfg) {
        return NULL;
    }
    approval_gate_t *gate = (approval_gate_t *)AIRY_CALLOC(1, sizeof(*gate));
    if (!gate) {
        return NULL;
    }
    gate->ctx = tool_approval_create(cfg);
    if (!gate->ctx) {
        AIRY_FREE(gate);
        return NULL;
    }
    gate_attach_bridge(gate);
    /* 读 AIRY_TOOL_APPROVAL_MODE 决定是否启用；创建失败仅禁用交互审批，
     * 不影响静态 fail-closed 审批。 */
    gate->interactive = interactive_approval_create();
    return gate;
}

approval_gate_t *approval_gate_create_default(void)
{
    tool_approval_config_t cfg;
    __builtin_memset(&cfg, 0, sizeof(cfg));
    cfg.agent_id = "tool_d";
    cfg.enable_safety_guard_chain = true;
    cfg.enable_audit_logging = true;
    cfg.permission_rules = NULL;
    return tool_approval_gate_create(&cfg);
}

void approval_gate_destroy(approval_gate_t *gate)
{
    if (!gate) {
        return;
    }
    if (gate->ctx) {
        tool_approval_destroy(gate->ctx);
        gate->ctx = NULL;
    }
    if (gate->bridge) {
        safety_guard_bridge_destroy(gate->bridge);
        gate->bridge = NULL;
    }
    if (gate->interactive) {
        interactive_approval_destroy(gate->interactive);
        gate->interactive = NULL;
    }
    AIRY_FREE(gate);
}

static void log_approved(const tool_metadata_t *meta, const tool_approval_detail_t *detail)
{
    SVC_LOG_INFO("Tool '%s' approved (decision=%d)", meta->name ? meta->name : "?",
                 (int)detail->decision);
}

typedef enum {
    INTERACTIVE_CONTINUE = 0, /* 未启用交互审批：回退静态拒绝理由 */
    INTERACTIVE_ALLOWED,
    INTERACTIVE_DENIED,
} interactive_step_t;

/* P0: tool 级交互审批（Claude Code 式授权提示）。启用时静态拒绝不再直接
 * fail-closed，而是入 pending 池阻塞等待 tool.approve 决议：
 *   - allow  -> 本次放行
 *   - always -> 放行并写入持久 ACL 规则
 *   - deny/超时 -> 拒绝（原因为 "User denied tool execution"） */
static interactive_step_t ask_via_interactive(approval_gate_t *gate, const char *agent_id,
                                              const tool_metadata_t *meta,
                                              const char *params_json, char **out_reason)
{
    if (!gate->interactive || !interactive_approval_is_enabled(gate->interactive)) {
        return INTERACTIVE_CONTINUE;
    }
    const char *agent = agent_id;
    if (!agent) {
        agent = tool_approval_get_agent_id(gate->ctx);
    }
    /* 交互等待只阻塞当前池 worker，其他会话与工具不受影响（等待上限由
     * 池预算覆盖）。 */
    airy_approval_outcome_t outcome = AIRY_APPROVAL_DENIED;
    char *request_id = interactive_approval_block(gate->interactive,
                                                  meta->name ? meta->name : "?",
                                                  agent ? agent : "unknown", params_json,
                                                  &outcome);
    if (request_id) {
        AIRY_FREE(request_id);
    }

    if (outcome == AIRY_APPROVAL_ALLOWED) {
        SVC_LOG_INFO("Tool '%s' approved by user (interactive, one-shot)",
                     meta->name ? meta->name : "?");
        return INTERACTIVE_ALLOWED;
    }
    if (outcome == AIRY_APPROVAL_ALWAYS) {
        SVC_LOG_INFO("Tool '%s' approved by user (interactive, always)",
                     meta->name ? meta->name : "?");
        /* 持久 ACL 规则（agent + 工具名 + allow）：后续相同调用直接通过
         * 静态审批。 */
        if (agent && meta->name) {
            int ar = daemon_security_add_acl_rule(agent, meta->name, true);
            if (ar != 0) {
                SVC_LOG_WARN("add_acl_rule('%s','%s') failed rc=%d", agent, meta->name, ar);
            }
        }
        return INTERACTIVE_ALLOWED;
    }
    SVC_LOG_ERROR("Tool '%s' denied by user (interactive) or timed out",
                  meta->name ? meta->name : "?");
    if (out_reason) {
        *out_reason = AIRY_STRDUP("User denied tool execution");
    }
    return INTERACTIVE_DENIED;
}

approval_gate_verdict_t approval_gate_ask(approval_gate_t *gate, const char *agent_id,
                                          const tool_metadata_t *meta,
                                          const char *params_json, char **out_reason)
{
    if (out_reason) {
        *out_reason = NULL;
    }
    if (!gate || !gate->ctx || !meta) {
        return APPROVAL_GATE_DENIED;
    }

    tool_approval_detail_t detail;
    int rc = tool_approval_check_for_agent(gate->ctx, agent_id, meta, params_json, &detail);
    if (rc == 0) {
        log_approved(meta, &detail);
        return APPROVAL_GATE_ALLOWED;
    }

    switch (ask_via_interactive(gate, agent_id, meta, params_json, out_reason)) {
    case INTERACTIVE_ALLOWED:
        log_approved(meta, &detail);
        return APPROVAL_GATE_ALLOWED;
    case INTERACTIVE_DENIED:
        return APPROVAL_GATE_DENIED;
    case INTERACTIVE_CONTINUE:
    default:
        break;
    }

    SVC_LOG_ERROR("Tool approval denied for '%s': %s", meta->name ? meta->name : "?",
                  detail.reason);
    if (out_reason && detail.reason[0]) {
        *out_reason = AIRY_STRDUP(detail.reason);
    }
    return APPROVAL_GATE_DENIED;
}

bool approval_gate_interactive_enabled(const approval_gate_t *gate)
{
    return gate && gate->interactive && interactive_approval_is_enabled(gate->interactive);
}

uint64_t approval_gate_interactive_budget_extra_ms(const approval_gate_t *gate)
{
    return approval_gate_interactive_enabled(gate) ? approval_timeout_ms() : 0;
}

char *approval_gate_interactive_pending_list(approval_gate_t *gate)
{
    if (!gate || !gate->interactive) {
        return NULL;
    }
    return interactive_approval_pending_list_json(gate->interactive);
}

int approval_gate_interactive_resolve(approval_gate_t *gate, const char *request_id,
                                      const char *decision)
{
    if (!gate || !gate->interactive) {
        return AIRY_ERR_NOT_FOUND;
    }
    return interactive_approval_resolve(gate->interactive, request_id, decision);
}
