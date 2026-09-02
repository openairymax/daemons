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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct safety_guard_bridge_s {
    safety_guard_bridge_config_t config;
    safety_guard_context_t *guard_ctx;
    char agent_id[128];
    bool initialized;

    uint64_t rate_limit_window_start;
    uint64_t rate_limit_call_count;

    uint64_t total_checks;
    uint64_t denied_count;
    uint64_t rate_limited;
};

static void bridge_config_defaults(safety_guard_bridge_config_t *cfg)
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
        bridge_config_defaults(&bridge->config);
        snprintf(bridge->agent_id, sizeof(bridge->agent_id), "%s", "unknown");
    }

    bridge->guard_ctx = safety_guard_create();
    if (!bridge->guard_ctx) {
        AIRY_LOG_WARN("C-L05: safety_guard_create failed, "
                      "guard checks will be skipped");
    }

    bridge->rate_limit_window_start = 0;
    bridge->rate_limit_call_count = 0;

    bridge->total_checks = 0;
    bridge->denied_count = 0;
    bridge->rate_limited = 0;
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
                  (unsigned long long)bridge->total_checks,
                  (unsigned long long)bridge->denied_count,
                  (unsigned long long)bridge->rate_limited);

    if (bridge->guard_ctx) {
        safety_guard_destroy(bridge->guard_ctx);
        bridge->guard_ctx = NULL;
    }

    AIRY_FREE(bridge);
}

/** @brief Get the current timestamp (ms). */
static uint64_t mono_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/**
 * @brief Check whether the rate-limit window needs resetting
 */
static void rate_window_sync(safety_guard_bridge_t *bridge)
{
    uint64_t now = mono_now_ms();
    if (bridge->rate_limit_window_start == 0) {
        bridge->rate_limit_window_start = now;
        return;
    }

    if (now - bridge->rate_limit_window_start >= 60000) {
        bridge->rate_limit_window_start = now;
        bridge->rate_limit_call_count = 0;
    }
}

/**
 * @brief Build a Cupolas safety event
 */
static void build_safety_event(safety_event_t *event, const tool_metadata_t *meta,
                               const char *params_json, const char *agent_id)
{
    __builtin_memset(event, 0, sizeof(*event));
    event->type = SAFETY_EVENT_EXECUTION_START;
    event->timestamp = mono_now_ms();

    if (meta) {
        snprintf(event->action, sizeof(event->action), "%s", meta->name ? meta->name : "unknown");
    }
    if (agent_id) {
        snprintf(event->subject, sizeof(event->subject), "%s", agent_id);
    }
    if (params_json) {
        event->context = params_json;
        event->context_size = strlen(params_json);
    }
}

/* Internal implementation: run the full guard chain with the given agent_id.
 * The permission guard (SAFETY_GUARD_PERMISSION) and the audit guard use the
 * passed-in agent_id; the other guards are agent-independent. */
static int bridge_check_impl(safety_guard_bridge_t *bridge, const char *agent_id,
                             const tool_metadata_t *meta, const char *params_json,
                             safety_guard_bridge_result_t *result)
{
    if (!bridge || !meta)
        return AIRY_ERR_INVALID_PARAM;

    if (!agent_id || !agent_id[0]) {
        agent_id = bridge->agent_id;
    }

    if (result) {
        __builtin_memset(result, 0, sizeof(*result));
    }

    bridge->total_checks++;

    const char *tool_name = meta->name ? meta->name : "unknown";
    int guard_chain_length = 0;
    int guards_executed = 0;

    if (bridge->config.enable_permission_guard) {
        guard_chain_length++;
        int perm_ret = daemon_check_tool_permission(agent_id, tool_name, "execute");
        if (perm_ret != 0) {
            if (result) {
                result->permission_passed = 0;
                snprintf(result->denial_reason, sizeof(result->denial_reason),
                         "Permission denied: agent '%s' cannot execute '%s'", agent_id, tool_name);
            }
            AIRY_LOG_WARN("C-L05: SAFETY_GUARD_PERMISSION denied for '%s' by '%s'", tool_name,
                          agent_id);
            bridge->denied_count++;
            return AIRY_ERR_PERMISSION_DENIED;
        }
        if (result)
            result->permission_passed = 1;
        guards_executed++;
    }

    if (bridge->config.enable_rate_limit_guard && bridge->config.rate_limit_per_minute > 0) {
        guard_chain_length++;
        rate_window_sync(bridge);

        bridge->rate_limit_call_count++;
        if (bridge->rate_limit_call_count > bridge->config.rate_limit_per_minute) {
            if (result) {
                result->rate_limit_passed = 0;
                snprintf(result->denial_reason, sizeof(result->denial_reason),
                         "Rate limit exceeded: %u/min for tool '%s'",
                         bridge->config.rate_limit_per_minute, tool_name);
            }
            AIRY_LOG_WARN("C-L05: SAFETY_GUARD_RATE_LIMIT exceeded for '%s' "
                          "(%llu calls)",
                          tool_name, (unsigned long long)bridge->rate_limit_call_count);
            bridge->rate_limited++;
            bridge->denied_count++;
            return AIRY_ERR_BUSY;
        }
        if (result)
            result->rate_limit_passed = 1;
        guards_executed++;
    } else if (bridge->config.enable_rate_limit_guard) {

        if (result)
            result->rate_limit_passed = 1;
    }

    if (bridge->config.enable_content_filter && params_json) {
        guard_chain_length++;

        if (bridge->config.denied_patterns && bridge->config.denied_patterns[0]) {
            char *patterns_copy = AIRY_STRDUP(bridge->config.denied_patterns);
            if (patterns_copy) {
                char *saveptr = NULL;
                char *token = strtok_r(patterns_copy, ",", &saveptr);
                while (token) {

                    while (*token == ' ')
                        token++;
                    if (*token && strstr(params_json, token)) {
                        if (result) {
                            result->content_filter_passed = 0;
                            snprintf(result->denial_reason, sizeof(result->denial_reason),
                                     "Content filter: denied pattern '%s' found "
                                     "in tool '%s' params",
                                     token, tool_name);
                        }
                        AIRY_LOG_WARN("C-L05: SAFETY_GUARD_CONTENT_FILTER "
                                      "denied pattern '%s' for '%s'",
                                      token, tool_name);
                        AIRY_FREE(patterns_copy);
                        bridge->denied_count++;
                        return AIRY_ERR_SEC_VIOLATION;
                    }
                    token = strtok_r(NULL, ",", &saveptr);
                }
                AIRY_FREE(patterns_copy);
            }
        }

        if (bridge->config.max_params_size > 0 && params_json) {
            size_t params_len = strlen(params_json);
            if (params_len > bridge->config.max_params_size) {
                if (result) {
                    result->content_filter_passed = 0;
                    snprintf(result->denial_reason, sizeof(result->denial_reason),
                             "Content filter: params size %zu exceeds max %u "
                             "for tool '%s'",
                             params_len, bridge->config.max_params_size, tool_name);
                }
                AIRY_LOG_WARN("C-L05: SAFETY_GUARD_CONTENT_FILTER size limit "
                              "exceeded for '%s' (%zu > %u)",
                              tool_name, params_len, bridge->config.max_params_size);
                bridge->denied_count++;
                return AIRY_ERR_SEC_VIOLATION;
            }
        }

        if (result)
            result->content_filter_passed = 1;
        guards_executed++;
    }

    if (bridge->config.enable_input_sanitization && params_json) {
        guard_chain_length++;

        char sanitized[4096] = {0};
        int san_ret = daemon_sanitize_tool_params(tool_name, params_json, sanitized,
                                                  sizeof(sanitized), sanitized, sizeof(sanitized));

        if (san_ret == 0) {
            if (result) {
                result->input_sanitized = 1;
                if (strcmp(params_json, sanitized) != 0) {
                    snprintf(result->sanitized_params, sizeof(result->sanitized_params), "%s",
                             sanitized);
                }
            }
        }
        if (result && result->input_sanitized) {
            guards_executed++;
        }
    }

    if (bridge->config.enable_resource_quota && bridge->guard_ctx) {
        guard_chain_length++;

        bool quota_allowed = true;
        int quota_ret = safety_guard_check_quota(bridge->guard_ctx, tool_name, 1, &quota_allowed);
        if (quota_ret == 0 && !quota_allowed) {
            if (result) {
                result->resource_quota_passed = 0;
                snprintf(result->denial_reason, sizeof(result->denial_reason),
                         "Resource quota exceeded for tool '%s'", tool_name);
            }
            AIRY_LOG_WARN("C-L05: SAFETY_GUARD_RESOURCE quota exceeded for '%s'", tool_name);
            bridge->denied_count++;
            return AIRY_ERR_SEC_QUOTA;
        }

        if (quota_ret == 0 && quota_allowed) {
            safety_guard_consume_quota(bridge->guard_ctx, tool_name, 1);
        }

        if (result)
            result->resource_quota_passed = 1;
        guards_executed++;
    }

    if (bridge->config.enable_audit_guard) {
        guard_chain_length++;

        if (bridge->guard_ctx) {
            safety_event_t event;
            safety_result_t audit_result;
            build_safety_event(&event, meta, params_json, agent_id);

            safety_guard_record_audit(bridge->guard_ctx, &event, &audit_result,
                                      "safety_guard_bridge");
        }

        daemon_audit_log_event("tool_d", "tool_execute_safety_guard", tool_name, 1, agent_id);

        if (result)
            result->audit_recorded = 1;
        guards_executed++;
    }

    if (result) {
        result->guard_chain_length = guard_chain_length;
        result->guards_executed = guards_executed;
    }

    return 0;
}

int safety_guard_bridge_check(safety_guard_bridge_t *bridge, const tool_metadata_t *meta,
                              const char *params_json, safety_guard_bridge_result_t *result)
{
    return bridge_check_impl(bridge, NULL, meta, params_json, result);
}

int safety_guard_bridge_check_for_agent(safety_guard_bridge_t *bridge, const char *agent_id,
                                        const tool_metadata_t *meta, const char *params_json,
                                        safety_guard_bridge_result_t *result)
{
    return bridge_check_impl(bridge, agent_id, meta, params_json, result);
}

int safety_guard_bridge_check_permission(safety_guard_bridge_t *bridge, const char *agent_id,
                                         const char *tool_name, const char *action)
{
    if (!bridge || !agent_id || !tool_name || !action)
        return AIRY_ERR_INVALID_PARAM;

    if (!bridge->config.enable_permission_guard)
        return 0;

    int ret = daemon_check_tool_permission(agent_id, tool_name, action);
    if (ret != 0) {
        AIRY_LOG_WARN("C-L05: Permission check failed for '%s' on '%s' (action=%s)", agent_id,
                      tool_name, action);
    }
    return ret;
}

int safety_guard_bridge_check_rate_limit(safety_guard_bridge_t *bridge, const char *tool_name)
{
    if (!bridge || !tool_name)
        return AIRY_ERR_INVALID_PARAM;

    if (!bridge->config.enable_rate_limit_guard || bridge->config.rate_limit_per_minute == 0) {
        return 0;
    }

    rate_window_sync(bridge);
    bridge->rate_limit_call_count++;

    if (bridge->rate_limit_call_count > bridge->config.rate_limit_per_minute) {
        bridge->rate_limited++;
        return AIRY_ERR_BUSY;
    }

    return 0;
}

int safety_guard_bridge_filter_content(safety_guard_bridge_t *bridge, const char *params_json,
                                       char *sanitized_params, size_t sanitized_size)
{
    if (!bridge || !params_json || !sanitized_params || sanitized_size == 0) {
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!bridge->config.enable_content_filter) {
        snprintf(sanitized_params, sanitized_size, "%s", params_json);
        return 0;
    }

    if (bridge->config.denied_patterns && bridge->config.denied_patterns[0]) {
        char *patterns_copy = AIRY_STRDUP(bridge->config.denied_patterns);
        if (patterns_copy) {
            char *saveptr = NULL;
            char *token = strtok_r(patterns_copy, ",", &saveptr);
            while (token) {
                while (*token == ' ')
                    token++;
                if (*token && strstr(params_json, token)) {
                    AIRY_FREE(patterns_copy);
                    return AIRY_ERR_SEC_VIOLATION;
                }
                token = strtok_r(NULL, ",", &saveptr);
            }
            AIRY_FREE(patterns_copy);
        }
    }

    if (bridge->config.max_params_size > 0) {
        size_t params_len = strlen(params_json);
        if (params_len > bridge->config.max_params_size) {
            return AIRY_ERR_SEC_VIOLATION;
        }
    }

    snprintf(sanitized_params, sanitized_size, "%s", params_json);
    return 0;
}

int safety_guard_bridge_audit_log(safety_guard_bridge_t *bridge, const char *event_type,
                                  const char *tool_name, int decision, const char *reason,
                                  const char *agent_id)
{
    if (!bridge || !event_type || !tool_name)
        return AIRY_ERR_INVALID_PARAM;

    if (!bridge->config.enable_audit_guard)
        return 0;

    if (bridge->guard_ctx) {
        safety_event_t event;
        safety_result_t result;
        __builtin_memset(&event, 0, sizeof(event));
        __builtin_memset(&result, 0, sizeof(result));

        event.type = SAFETY_EVENT_EXECUTION_COMPLETE;
        event.timestamp = mono_now_ms();
        snprintf(event.action, sizeof(event.action), "%s", tool_name);
        snprintf(event.subject, sizeof(event.subject), "%s",
                 agent_id ? agent_id : bridge->agent_id);

        result.decision = (decision == 0) ? SAFETY_DECISION_ALLOW : SAFETY_DECISION_DENY;
        if (reason) {
            snprintf(result.reason, sizeof(result.reason), "%s", reason);
        }

        safety_guard_record_audit(bridge->guard_ctx, &event, &result, "safety_guard_bridge");
    }

    int audit_decision = (decision == 0) ? 1 : 0;
    daemon_audit_log_event("tool_d", event_type, tool_name, audit_decision,
                           agent_id ? agent_id : bridge->agent_id);

    return 0;
}

void safety_guard_bridge_get_stats(safety_guard_bridge_t *bridge, uint64_t *out_total_checks,
                                   uint64_t *out_denied_count, uint64_t *out_rate_limited)
{
    if (!bridge) {
        if (out_total_checks)
            *out_total_checks = 0;
        if (out_denied_count)
            *out_denied_count = 0;
        if (out_rate_limited)
            *out_rate_limited = 0;
        return;
    }

    if (out_total_checks)
        *out_total_checks = bridge->total_checks;
    if (out_denied_count)
        *out_denied_count = bridge->denied_count;
    if (out_rate_limited)
        *out_rate_limited = bridge->rate_limited;
}