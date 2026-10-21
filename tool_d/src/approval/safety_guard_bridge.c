// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file safety_guard_bridge.c
 * @brief C-L05: Cupolas SafetyGuard -> tool_d bridge-layer implementation.
 *
 * Bridges the cupolas security dome's safety_guard_check_chain() API into
 * the tool_d tool-approval flow, implementing permission checks, rate
 * limiting, content filtering and other security controls for the 6 guard
 * types.
 *
 * Guard-type mapping:
 *   SAFETY_GUARD_PERMISSION   -> RBAC permission check
 *   SAFETY_GUARD_RATE_LIMIT   -> tool-call rate limiting
 *   SAFETY_GUARD_CONTENT_FILTER -> input content filtering
 *   SAFETY_GUARD_INPUT        -> parameter sanitization
 *   SAFETY_GUARD_RESOURCE     -> resource-quota check
 *   SAFETY_GUARD_AUDIT        -> audit-log recording
 *
 * 0.1.19 L1 收敛：检查主体拆为栈上上下文加逐 guard 阶段函数，拒绝路径统一
 * 经 sgb_deny() 出口，denial_reason、告警与 denied_count 三处语义同源。
 */

#include "safety_guard_bridge.h"
#include "safety_guard.h"

#include "daemon_errors.h"
#include "daemon_security.h"
#include "error.h"
#include "logger.h"
#include "airy_memory.h"
#include "string_compat.h"

#include "platform.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct safety_guard_bridge_s {
    safety_guard_bridge_config_t config;
    safety_guard_context_t *guard_ctx;
    char agent_id[128];
    bool initialized;

    /* 限流窗口专用小临界区：窗口起止与调用计数的读-改-写必须原子，
     * 临界区内不得调用 cupolas/ACL 等外部接口（锁序安全）。 */
    airy_mtx_t rl_lock;
    uint64_t rate_limit_window_start;
    uint64_t rate_limit_call_count;

    /* R1-a (0.1.17): 执行面 worker 池并发后桥接检查可多线程同时进入，
     * 统计计数器改为原子量（仅统计用途，relaxed 序即可）。 */
    _Atomic uint64_t total_checks;
    _Atomic uint64_t denied_count;
    _Atomic uint64_t rate_limited;
};

typedef struct {
    safety_guard_bridge_t *bridge;
    const char *agent;
    const char *tool;
    const char *params;
    safety_guard_bridge_result_t *res;
    int chain_len;
    int executed;
} sgb_chk_t;

void sgb_cfg_defaults(safety_guard_bridge_config_t *cfg)
{
    if (!cfg)
        return;
    cfg->enable_permission_guard = true;
    cfg->enable_rate_limit_guard = true;
    cfg->enable_content_filter = true;
    cfg->enable_input_sanitization = true;
    cfg->enable_resource_quota = true;
    cfg->enable_audit_guard = true;
    cfg->rate_limit_per_minute = 0;
    cfg->max_params_size = 0;
    cfg->denied_patterns = NULL;
    cfg->agent_id = "unknown";
}

safety_guard_bridge_t *safety_guard_bridge_create(const safety_guard_bridge_config_t *config)
{
    safety_guard_bridge_t *bridge =
        (safety_guard_bridge_t *)AIRY_CALLOC(1, sizeof(safety_guard_bridge_t));
    if (!bridge) {
        AIRY_LOG_ERROR("C-L05: safety_guard_bridge_create: OOM");
        return NULL;
    }

    if (config) {
        bridge->config = *config;
        if (config->agent_id) {
            snprintf(bridge->agent_id, sizeof(bridge->agent_id), "%s", config->agent_id);
        } else {
            snprintf(bridge->agent_id, sizeof(bridge->agent_id), "%s", "unknown");
        }
    } else {
        sgb_cfg_defaults(&bridge->config);
        snprintf(bridge->agent_id, sizeof(bridge->agent_id), "%s", "unknown");
    }

    bridge->guard_ctx = safety_guard_create();
    if (!bridge->guard_ctx) {
        AIRY_LOG_WARN("C-L05: safety_guard_create failed, "
                      "guard checks will be skipped");
    }

    if (airy_mtx_init(&bridge->rl_lock) != 0) {
        AIRY_LOG_ERROR("C-L05: safety_guard_bridge_create: rl_lock init failed");
        if (bridge->guard_ctx) {
            safety_guard_destroy(bridge->guard_ctx);
        }
        AIRY_FREE(bridge);
        return NULL;
    }

    bridge->initialized = true;

    AIRY_LOG_INFO("C-L05: SafetyGuard bridge created (permission=%d, rate_limit=%d, "
                  "content_filter=%d, input_san=%d, resource=%d, audit=%d, rate_limit/min=%u)",
                  bridge->config.enable_permission_guard, bridge->config.enable_rate_limit_guard,
                  bridge->config.enable_content_filter, bridge->config.enable_input_sanitization,
                  bridge->config.enable_resource_quota, bridge->config.enable_audit_guard,
                  bridge->config.rate_limit_per_minute);
    return bridge;
}

void safety_guard_bridge_destroy(safety_guard_bridge_t *bridge)
{
    if (!bridge)
        return;

    AIRY_LOG_INFO("C-L05: SafetyGuard bridge destroyed "
                  "(checks=%llu denied=%llu rate_limited=%llu)",
                  (unsigned long long)atomic_load_explicit(&bridge->total_checks,
                                                           memory_order_relaxed),
                  (unsigned long long)atomic_load_explicit(&bridge->denied_count,
                                                           memory_order_relaxed),
                  (unsigned long long)atomic_load_explicit(&bridge->rate_limited,
                                                           memory_order_relaxed));

    if (bridge->guard_ctx) {
        safety_guard_destroy(bridge->guard_ctx);
        bridge->guard_ctx = NULL;
    }

    airy_mtx_destroy(&bridge->rl_lock);
    AIRY_FREE(bridge);
}

/* 统一拒绝出口：各 *_passed 字段已由入口 memset 清零，此处只负责写拒绝
 * 原因、告警与拒绝计数，避免五处同构代码各自演化。 */
static int sgb_deny(sgb_chk_t *c, int code, const char *tag, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (c->res)
        snprintf(c->res->denial_reason, sizeof(c->res->denial_reason), "%s", msg);
    AIRY_LOG_WARN("C-L05: %s: %s", tag, msg);
    atomic_fetch_add_explicit(&c->bridge->denied_count, 1, memory_order_relaxed);
    return code;
}

/* 与 sgb_deny 对称的通过出口：置位该 guard 的通过标记并累计已执行数。 */
static int sgb_pass(sgb_chk_t *c, int *passed)
{
    if (passed)
        *passed = 1;
    c->executed++;
    return 0;
}

static int sgb_perm(sgb_chk_t *c)
{
    if (!c->bridge->config.enable_permission_guard)
        return 0;

    c->chain_len++;
    if (daemon_check_tool_permission(c->agent, c->tool, "execute") != 0) {
        return sgb_deny(c, AIRY_ERR_PERMISSION_DENIED, "SAFETY_GUARD_PERMISSION",
                        "Permission denied: agent '%s' cannot execute '%s'", c->agent, c->tool);
    }
    return sgb_pass(c, c->res ? &c->res->permission_passed : NULL);
}

static int sgb_rate(sgb_chk_t *c)
{
    bool allowed;
    uint64_t calls, now;

    if (!c->bridge->config.enable_rate_limit_guard)
        return 0;

    if (c->bridge->config.rate_limit_per_minute == 0) {
        if (c->res)
            c->res->rate_limit_passed = 1;
        return 0;
    }

    c->chain_len++;

    /* 窗口重置 + 计数 + 超限判定必须在同一临界区内完成，
     * 否则并发下限流会失真（超发或误限）。 */
    airy_mtx_lock(&c->bridge->rl_lock);
    now = airy_time_ms();
    if (c->bridge->rate_limit_window_start == 0)
        c->bridge->rate_limit_window_start = now;
    if (now - c->bridge->rate_limit_window_start >= 60000) {
        c->bridge->rate_limit_window_start = now;
        c->bridge->rate_limit_call_count = 0;
    }
    calls = ++c->bridge->rate_limit_call_count;
    allowed = calls <= c->bridge->config.rate_limit_per_minute;
    airy_mtx_unlock(&c->bridge->rl_lock);

    if (!allowed) {
        atomic_fetch_add_explicit(&c->bridge->rate_limited, 1, memory_order_relaxed);
        return sgb_deny(c, AIRY_ERR_BUSY, "SAFETY_GUARD_RATE_LIMIT",
                        "Rate limit exceeded: %u/min for tool '%s' (%llu calls)",
                        c->bridge->config.rate_limit_per_minute, c->tool,
                        (unsigned long long)calls);
    }
    return sgb_pass(c, c->res ? &c->res->rate_limit_passed : NULL);
}

/* 在逗号分隔的黑名单词表中查找首个出现在参数中的词；命中即经统一出口拒绝。 */
static int sgb_scan(sgb_chk_t *c, const char *denied)
{
    char *copy = AIRY_STRDUP(denied), *save = NULL, *tok;
    int rc = 0;

    if (!copy)
        return 0;

    tok = strtok_r(copy, ",", &save);
    while (tok) {
        while (*tok == ' ')
            tok++;
        if (*tok && strstr(c->params, tok)) {
            rc = sgb_deny(c, AIRY_ERR_SEC_VIOLATION, "SAFETY_GUARD_CONTENT_FILTER",
                          "Content filter: denied pattern '%s' found in tool '%s' params", tok,
                          c->tool);
            break;
        }
        tok = strtok_r(NULL, ",", &save);
    }
    AIRY_FREE(copy);
    return rc;
}

static int sgb_filter(sgb_chk_t *c)
{
    if (!c->bridge->config.enable_content_filter || !c->params)
        return 0;

    const char *denied = c->bridge->config.denied_patterns;
    uint32_t max_size = c->bridge->config.max_params_size;
    size_t params_len = strlen(c->params);
    int rc = 0;

    c->chain_len++;

    if (denied && denied[0])
        rc = sgb_scan(c, denied);

    if (rc == 0 && max_size > 0 && params_len > max_size) {
        rc = sgb_deny(c, AIRY_ERR_SEC_VIOLATION, "SAFETY_GUARD_CONTENT_FILTER",
                      "Content filter: params size %zu exceeds max %u for tool '%s'", params_len,
                      max_size, c->tool);
    }
    if (rc == 0)
        rc = sgb_pass(c, c->res ? &c->res->content_filter_passed : NULL);
    return rc;
}

static int sgb_input(sgb_chk_t *c)
{
    char sanitized_tool[128] = {0};
    char sanitized_params[4096] = {0};

    if (!c->bridge->config.enable_input_sanitization || !c->params)
        return 0;

    c->chain_len++;
    if (daemon_sanitize_tool_params(c->tool, c->params, sanitized_tool, sizeof(sanitized_tool),
                                    sanitized_params, sizeof(sanitized_params)) != 0)
        return 0;

    if (c->res && strcmp(c->params, sanitized_params) != 0) {
        snprintf(c->res->sanitized_params, sizeof(c->res->sanitized_params), "%s",
                 sanitized_params);
    }
    return sgb_pass(c, c->res ? &c->res->input_sanitized : NULL);
}

static int sgb_quota(sgb_chk_t *c)
{
    bool allowed = true;

    if (!c->bridge->config.enable_resource_quota || !c->bridge->guard_ctx)
        return 0;

    c->chain_len++;
    int quota_ret = safety_guard_check_quota(c->bridge->guard_ctx, c->tool, 1, &allowed);
    if (quota_ret == 0 && !allowed) {
        return sgb_deny(c, AIRY_ERR_SEC_QUOTA, "SAFETY_GUARD_RESOURCE",
                        "Resource quota exceeded for tool '%s'", c->tool);
    }
    if (quota_ret == 0 && allowed)
        safety_guard_consume_quota(c->bridge->guard_ctx, c->tool, 1);

    return sgb_pass(c, c->res ? &c->res->resource_quota_passed : NULL);
}

static int sgb_audit(sgb_chk_t *c)
{
    if (!c->bridge->config.enable_audit_guard)
        return 0;

    c->chain_len++;
    if (c->bridge->guard_ctx) {
        safety_event_t event;
        safety_result_t audit_result;
        __builtin_memset(&event, 0, sizeof(event));
        event.type = SAFETY_EVENT_EXECUTION_START;
        event.timestamp = airy_time_ms();
        snprintf(event.action, sizeof(event.action), "%s", c->tool);
        snprintf(event.subject, sizeof(event.subject), "%s", c->agent);
        if (c->params) {
            event.context = c->params;
            event.context_size = strlen(c->params);
        }
        safety_guard_record_audit(c->bridge->guard_ctx, &event, &audit_result,
                                  "safety_guard_bridge");
    }

    /* result=0 → SUCCESS。此处为 guard 链全部通过后的成功路径；原硬编码
     * 1 使每次工具执行都被审计为 FAILED（假阳性遥测，误导排障）。 */
    daemon_audit_log_event("tool_d", "tool_execute_safety_guard", c->tool, 0, c->agent);

    return sgb_pass(c, c->res ? &c->res->audit_recorded : NULL);
}

/* Run the full guard chain with the given agent_id. The permission guard
 * (SAFETY_GUARD_PERMISSION) and the audit guard use the passed-in agent_id;
 * the other guards are agent-independent. */
int safety_guard_bridge_check_for_agent(safety_guard_bridge_t *bridge, const char *agent_id,
                                        const tool_metadata_t *meta, const char *params_json,
                                        safety_guard_bridge_result_t *result)
{
    static int (*const stages[])(sgb_chk_t *) = {sgb_perm, sgb_rate,  sgb_filter,
                                                 sgb_input, sgb_quota, sgb_audit};
    sgb_chk_t c;
    int rc = 0;
    size_t i;

    if (!bridge || !meta)
        return AIRY_ERR_INVALID_PARAM;

    __builtin_memset(&c, 0, sizeof(c));
    c.bridge = bridge;
    c.agent = (agent_id && agent_id[0]) ? agent_id : bridge->agent_id;
    c.tool = meta->name ? meta->name : "unknown";
    c.params = params_json;
    c.res = result;

    if (result)
        __builtin_memset(result, 0, sizeof(*result));

    atomic_fetch_add_explicit(&bridge->total_checks, 1, memory_order_relaxed);

    for (i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        rc = stages[i](&c);
        if (rc != 0)
            break;
    }

    /* 拒绝路径同样回填链长与执行数：排障需要知道链在第几段终止。 */
    if (result) {
        result->guard_chain_length = c.chain_len;
        result->guards_executed = c.executed;
    }
    return rc;
}
