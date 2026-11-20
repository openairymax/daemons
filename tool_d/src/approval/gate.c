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
#include "job/tool_audit.h"
#include "tool_approval.h"
#include "safety_guard_bridge.h"
#include "tool_interactive_approval.h"

#include "daemon_security.h"
#include "svc_logger.h"
#include "airy_memory.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    sgb_cfg_defaults(&bridge_cfg);
    bridge_cfg.agent_id = "tool_d";

    gate->bridge = safety_guard_bridge_create(&bridge_cfg);
    if (gate->bridge) {
        SVC_LOG_INFO("SafetyGuard bridge created for approval gate");
    } else {
        SVC_LOG_WARN("Failed to create SafetyGuard bridge, falling back to local checks");
    }
    tool_approval_set_safety_guard_bridge(gate->ctx, gate->bridge);
}

approval_gate_t *gate_create(const tool_approval_config_t *cfg)
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

approval_gate_t *gate_create_default(void)
{
    tool_approval_config_t cfg;
    __builtin_memset(&cfg, 0, sizeof(cfg));
    cfg.agent_id = "tool_d";
    cfg.enable_safety_guard_chain = true;
    cfg.enable_audit_logging = true;
    cfg.permission_rules = NULL;
    return gate_create(&cfg);
}

void gate_destroy(approval_gate_t *gate)
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

static bool gate_int_enabled(const approval_gate_t *gate)
{
    return gate && gate->interactive && interactive_approval_is_enabled(gate->interactive);
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
    if (!gate_int_enabled(gate))
        return INTERACTIVE_CONTINUE;
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

/* D-9 高危回退档：warn 只降级为告警（不得作为发版默认），require 为默认。 */
static int risk_warn_mode(void)
{
    const char *mode = getenv("AIRY_TOOL_RISK_APPROVAL_MODE");
    return mode && strcmp(mode, "warn") == 0;
}

static void approval_set(char *approval, size_t cap, const char *value)
{
    if (approval && cap)
        snprintf(approval, cap, "%s", value);
}

approval_gate_verdict_t gate_ask(approval_gate_t *gate, const char *agent_id,
                                          const tool_metadata_t *meta,
                                          const char *params_json, char **out_reason)
{
    return gate_ask_ex(gate, agent_id, meta, params_json, 0, NULL, 0, out_reason);
}

/* 静态审批通过：回填 approval 来源，高危需区分显式放行与 warn 档。 */
static approval_gate_verdict_t gate_acl_ok(const tool_metadata_t *meta,
                                           const tool_approval_detail_t *detail, char *approval,
                                           size_t cap, tool_audit_risk_t risk, int allow_dangerous)
{
    int high = (risk == TOOL_AUDIT_RISK_HIGH);
    approval_set(approval, cap, high ? (allow_dangerous ? "allow_dangerous" : "warn") : "acl");
    log_approved(meta, detail);
    return APPROVAL_GATE_ALLOWED;
}

/* 交互审批统一入口：返回 1=已放行 0=继续（未决） -1=已拒绝，
 * 放行分支的审批来源标记与审计日志只在此处落地一次。 */
static int gate_ask(approval_gate_t *gate, const char *agent_id, const tool_metadata_t *meta,
                    const char *params_json, char *approval, size_t cap,
                    const tool_approval_detail_t *detail, char **out_reason)
{
    switch (ask_via_interactive(gate, agent_id, meta, params_json, out_reason)) {
    case INTERACTIVE_ALLOWED:
        approval_set(approval, cap, "interactive");
        log_approved(meta, detail);
        return 1;
    case INTERACTIVE_DENIED:
        return -1;
    default:
        return 0;
    }
}

/* D-9 高危逐次审批：未显式放行且非 warn 档时必须交互确认，否则 fail-closed。 */
static approval_gate_verdict_t gate_risk_high(approval_gate_t *gate, const char *agent_id,
                                              const tool_metadata_t *meta,
                                              const char *params_json, char *approval, size_t cap,
                                              const char *risk_reason,
                                              const tool_approval_detail_t *detail,
                                              char **out_reason)
{
    int r = gate_ask(gate, agent_id, meta, params_json, approval, cap, detail, out_reason);
    if (r > 0)
        return APPROVAL_GATE_ALLOWED;
    if (r < 0)
        return APPROVAL_GATE_DENIED;

    SVC_LOG_ERROR("High-risk tool '%s' denied (fail-closed): %s; pass allow_dangerous "
                  "or set AIRY_TOOL_RISK_APPROVAL_MODE=warn",
                  meta->id ? meta->id : "?", risk_reason[0] ? risk_reason : "policy");
    if (out_reason) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "High-risk tool '%s' requires approval (%s); pass allow_dangerous",
                 meta->id ? meta->id : "?", risk_reason[0] ? risk_reason : "policy");
        *out_reason = AIRY_STRDUP(msg);
    }
    return APPROVAL_GATE_DENIED;
}

/* 静态审批拒绝：交互审批开启时给用户一次放行机会，否则按拒绝理由收尾。 */
static approval_gate_verdict_t gate_acl_deny(approval_gate_t *gate, const char *agent_id,
                                             const tool_metadata_t *meta, const char *params_json,
                                             char *approval, size_t cap,
                                             const tool_approval_detail_t *detail,
                                             char **out_reason)
{
    int r = gate_ask(gate, agent_id, meta, params_json, approval, cap, detail, out_reason);
    if (r > 0)
        return APPROVAL_GATE_ALLOWED;
    if (r < 0)
        return APPROVAL_GATE_DENIED;

    SVC_LOG_ERROR("Tool approval denied for '%s': %s", meta->name ? meta->name : "?",
                  detail->reason);
    if (out_reason && detail->reason[0])
        *out_reason = AIRY_STRDUP(detail->reason);
    return APPROVAL_GATE_DENIED;
}

approval_gate_verdict_t gate_ask_ex(approval_gate_t *gate, const char *agent_id,
                                    const tool_metadata_t *meta, const char *params_json,
                                    int allow_dangerous, char *approval, size_t cap,
                                    char **out_reason)
{
    char risk_reason[64];

    if (out_reason)
        *out_reason = NULL;
    approval_set(approval, cap, "none");
    if (!gate || !gate->ctx || !meta)
        return APPROVAL_GATE_DENIED;

    tool_audit_risk_t risk = tool_audit_risk(meta->id, params_json, risk_reason,
                                             sizeof(risk_reason));
    tool_approval_detail_t detail;
    int rc = tool_approval_check_for_agent(gate->ctx, agent_id, meta, params_json, &detail);

    if (risk == TOOL_AUDIT_RISK_HIGH && !allow_dangerous && !risk_warn_mode())
        return gate_risk_high(gate, agent_id, meta, params_json, approval, cap, risk_reason,
                              &detail, out_reason);
    if (risk == TOOL_AUDIT_RISK_HIGH)
        SVC_LOG_WARN("High-risk tool '%s' allowed without per-call approval (%s): %s",
                     meta->id ? meta->id : "?",
                     allow_dangerous ? "allow_dangerous" : "mode=warn",
                     risk_reason[0] ? risk_reason : "policy");
    if (rc == 0)
        return gate_acl_ok(meta, &detail, approval, cap, risk, allow_dangerous);
    return gate_acl_deny(gate, agent_id, meta, params_json, approval, cap, &detail, out_reason);
}

uint64_t gate_int_budget_ms(const approval_gate_t *gate)
{
    return gate_int_enabled(gate) ? approval_timeout_ms() : 0;
}

char *gate_int_pending(approval_gate_t *gate)
{
    if (!gate || !gate->interactive) {
        return NULL;
    }
    return interactive_approval_pending_list_json(gate->interactive);
}

int gate_int_resolve(approval_gate_t *gate, const char *request_id,
                                      const char *decision)
{
    if (!gate || !gate->interactive) {
        return AIRY_ERR_NOT_FOUND;
    }
    return interactive_approval_resolve(gate->interactive, request_id, decision);
}
