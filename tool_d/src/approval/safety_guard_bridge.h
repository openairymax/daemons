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

#include "airy_abi.h"

AIRY_ABI_BEGIN


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
 * @brief Run the full SafetyGuard guard-chain check as the given agent.
 *
 * The permission/audit guards use the passed agent_id instead of the
 * bridge's default one. Used to pass through the real agent identity per
 * request (e.g. agent_d child process coding_v1), so ACLs are evaluated
 * against the real subject and unauthorized tools enter interactive
 * approval.
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

AIRY_ABI_END

#endif /* AIRY_RT_SAFETY_GUARD_BRIDGE_H */
