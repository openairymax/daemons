/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file safety_guard_bridge.h
 * @brief C-L05: Cupolas SafetyGuard -> tool_d bridge layer.
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

#ifndef AIRY_RT_SAFETY_GUARD_BRIDGE_H
#define AIRY_RT_SAFETY_GUARD_BRIDGE_H

#include "tool_approval.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct safety_guard_bridge_s safety_guard_bridge_t;


typedef struct {
    bool enable_permission_guard;
    bool enable_rate_limit_guard;
    bool enable_content_filter;
    bool enable_input_sanitization;
    bool enable_resource_quota;
    bool enable_audit_guard;
    uint32_t rate_limit_per_minute;
    uint32_t max_params_size;
    const char *denied_patterns;
    const char *agent_id; /**< Agent ID */
} safety_guard_bridge_config_t;


typedef struct {
    int permission_passed;
    int rate_limit_passed;
    int content_filter_passed;
    int input_sanitized;
    int resource_quota_passed;
    int audit_recorded;
    char denial_reason[256];
    char sanitized_params[4096];
    int guard_chain_length;
    int guards_executed;
} safety_guard_bridge_result_t;


/**
 * @brief Create the SafetyGuard bridge layer.
 * @param config Bridge config (NULL = defaults: all guards enabled)
 * @return Bridge handle, NULL on failure
 * @ownership return: OWNER
 */
safety_guard_bridge_t *safety_guard_bridge_create(const safety_guard_bridge_config_t *config);

/**
 * @brief Destroy the SafetyGuard bridge layer.
 * @param bridge Bridge handle
 * @ownership bridge: TRANSFER
 */
void safety_guard_bridge_destroy(safety_guard_bridge_t *bridge);


/**
 * @brief C-L05: run the full SafetyGuard guard-chain check.
 *
 * Runs the 6 guard types in sequence:
 *   1. SAFETY_GUARD_PERMISSION   -> RBAC permission check
 *   2. SAFETY_GUARD_RATE_LIMIT   -> rate limiting
 *   3. SAFETY_GUARD_CONTENT_FILTER -> content filtering
 *   4. SAFETY_GUARD_INPUT        -> parameter sanitization
 *   5. SAFETY_GUARD_RESOURCE     -> resource quota
 *   6. SAFETY_GUARD_AUDIT        -> audit logging
 *
 * Any guard returning DENY -> stops immediately and returns the denial.
 *
 * @param bridge Bridge handle
 * @param meta Tool metadata
 * @param params_json Raw parameter JSON
 * @param result Output check result
 * @return 0 all passed, non-zero denied
 * @ownership bridge: BORROW, meta: BORROW, params_json: BORROW, result: BORROW
 */
int safety_guard_bridge_check(safety_guard_bridge_t *bridge, const tool_metadata_t *meta,
                              const char *params_json, safety_guard_bridge_result_t *result);

/**
 * @brief Run the full SafetyGuard guard-chain check as the given agent.
 *
 * Equivalent to safety_guard_bridge_check, but the permission/audit guards
 * use the passed agent_id instead of the bridge's default one. Used to
 * pass through the real agent identity per request (e.g. agent_d child
 * process coding_v1), so ACLs are evaluated against the real subject and
 * unauthorized tools enter interactive approval.
 *
 * @param bridge Bridge handle
 * @param agent_id Agent ID of this request (NULL = fall back to bridge default)
 * @param meta Tool metadata
 * @param params_json Raw parameter JSON
 * @param result Output check result
 * @return 0 all passed, non-zero denied
 */
int safety_guard_bridge_check_for_agent(safety_guard_bridge_t *bridge, const char *agent_id,
                                        const tool_metadata_t *meta, const char *params_json,
                                        safety_guard_bridge_result_t *result);

/**
 * @brief Run only the permission-guard check.
 * @param bridge Bridge handle
 * @param agent_id Agent ID
 * @param tool_name Tool name
 * @param action Operation ("execute"/"register"/"list")
 * @return 0 passed, non-zero denied
 */
int safety_guard_bridge_check_permission(safety_guard_bridge_t *bridge, const char *agent_id,
                                         const char *tool_name, const char *action);

/**
 * @brief Run only the rate-limit check.
 * @param bridge Bridge handle
 * @param tool_name Tool name
 * @return 0 passed, non-zero over limit
 */
int safety_guard_bridge_check_rate_limit(safety_guard_bridge_t *bridge, const char *tool_name);

/**
 * @brief Run only the content-filter check.
 * @param bridge Bridge handle
 * @param params_json Parameter JSON
 * @param sanitized_params Output sanitized parameters
 * @param sanitized_size Output buffer size
 * @return 0 passed, non-zero filtered
 */
int safety_guard_bridge_filter_content(safety_guard_bridge_t *bridge, const char *params_json,
                                       char *sanitized_params, size_t sanitized_size);


/**
 * @brief Record an audit-log event.
 * @param bridge Bridge handle
 * @param event_type Event type
 * @param tool_name Tool name
 * @param decision Decision result
 * @param reason Reason
 * @param agent_id Agent ID
 * @return 0 on success
 */
int safety_guard_bridge_audit_log(safety_guard_bridge_t *bridge, const char *event_type,
                                  const char *tool_name, int decision, const char *reason,
                                  const char *agent_id);

/**
 * @brief Get bridge statistics.
 * @param bridge Bridge handle
 * @param out_total_checks Output total check count
 * @param out_denied_count Output denied count
 * @param out_rate_limited Output rate-limited count
 */
void safety_guard_bridge_get_stats(safety_guard_bridge_t *bridge, uint64_t *out_total_checks,
                                   uint64_t *out_denied_count, uint64_t *out_rate_limited);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_SAFETY_GUARD_BRIDGE_H */