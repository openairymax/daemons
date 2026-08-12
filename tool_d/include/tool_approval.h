/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_approval.h
 * @brief C-L05: Cupolas SafetyGuard -> tool_d tool-approval adapter.
 *
 * Runs permission checks and parameter sanitization through the Cupolas
 * security dome before tool execution. The integration point is
 * tool_executor_run() in executor.c.
 *
 * Approval flow:
 *   1. Parameter sanitization (daemon_sanitize_tool_params)
 *   2. Permission check (daemon_check_tool_permission)
 *   3. SafetyGuard chain check (safety_guard_check_chain, optional)
 *   4. Audit recording (daemon_audit_log_event)
 */

#ifndef AIRY_RT_TOOL_APPROVAL_H
#define AIRY_RT_TOOL_APPROVAL_H

#include "tool_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


typedef struct safety_guard_bridge_s safety_guard_bridge_t;

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Tool-approval result. */
typedef enum {
    TOOL_APPROVAL_ALLOWED = 0,
    TOOL_APPROVAL_DENIED,
    TOOL_APPROVAL_SANITIZED,
    TOOL_APPROVAL_PENDING_AUDIT
} tool_approval_result_t;

/** @brief Tool-approval context. */
typedef struct tool_approval_ctx tool_approval_ctx_t;

/** @brief Tool-approval config. */
typedef struct {
    const char *agent_id;
    bool enable_safety_guard_chain;
    bool enable_audit_logging;
    const char *permission_rules;
} tool_approval_config_t;

/** @brief Detailed approval result. */
typedef struct {
    tool_approval_result_t decision;
    char reason[256];
    char sanitized_params[4096];
    int permission_check_passed;
    int safety_guard_passed;
    int params_were_sanitized;
} tool_approval_detail_t;


/**
 * @brief Create a tool-approval context.
 * @param cfg Approval config (NULL = defaults)
 * @return Approval context, NULL on failure
 *
 * @ownership return: OWNER
 */
tool_approval_ctx_t *tool_approval_create(const tool_approval_config_t *cfg);

/**
 * @brief Destroy a tool-approval context.
 * @param ctx Approval context
 *
 * @ownership ctx: TRANSFER
 */
void tool_approval_destroy(tool_approval_ctx_t *ctx);


/**
 * @brief C-L05: approve a tool-execution request.
 *
 * Call before tool execution; runs, in order:
 *   1. Parameter-sanitization check
 *   2. Permission check (calls daemon_check_tool_permission)
 *   3. SafetyGuard chain check (optional, calls safety_guard_check_chain)
 *   4. Audit-log recording
 *
 * @param ctx Approval context
 * @param meta Tool metadata
 * @param params_json Raw parameter JSON
 * @param detail Output approval detail
 * @return 0 on success (approved), non-zero denied
 *
 * @ownership ctx: BORROW, meta: BORROW, params_json: BORROW, detail: BORROW
 */
int tool_approval_check(tool_approval_ctx_t *ctx, const tool_metadata_t *meta,
                        const char *params_json, tool_approval_detail_t *detail);

/**
 * @brief C-L05: approve a tool-execution request as the given agent.
 *
 * Equivalent to tool_approval_check, but permission checks and audit use
 * the passed agent_id as the subject (instead of the context's default
 * agent). Used to pass through the real agent identity per request, so
 * unauthorized tool calls enter the interactive-approval flow.
 *
 * @param ctx Approval context
 * @param agent_id Agent ID of this request (NULL/empty = fall back to context default)
 * @param meta Tool metadata
 * @param params_json Raw parameter JSON
 * @param detail Output approval detail
 * @return 0 on success (approved), non-zero denied
 */
int tool_approval_check_for_agent(tool_approval_ctx_t *ctx, const char *agent_id,
                                  const tool_metadata_t *meta, const char *params_json,
                                  tool_approval_detail_t *detail);

/**
 * @brief Run only parameter sanitization (no permission check).
 *
 * @param ctx Approval context
 * @param tool_name Tool name
 * @param params_json Raw parameters
 * @param sanitized_params Output sanitized parameters
 * @param sanitized_size Output buffer size
 * @return 0 on success, non-zero on failure
 *
 * @ownership ctx: BORROW, tool_name: BORROW, params_json: BORROW
 * @ownership sanitized_params: caller provides buffer
 */
int tool_approval_sanitize_params(tool_approval_ctx_t *ctx, const char *tool_name,
                                  const char *params_json, char *sanitized_params,
                                  size_t sanitized_size);

/**
 * @brief Get statistics of the last approvals.
 *
 * @param ctx Approval context
 * @param out_total_checks Output total check count
 * @param out_denied_count Output denied count
 * @param out_sanitized_count Output sanitized count
 *
 * @ownership ctx: BORROW
 */
void tool_approval_get_stats(tool_approval_ctx_t *ctx, uint64_t *out_total_checks,
                             uint64_t *out_denied_count, uint64_t *out_sanitized_count);

/**
 * @brief C-L05: set the SafetyGuard bridge layer.
 *
 * Injects safety_guard_bridge into the approval context so
 * tool_approval_check() can run the full 6-guard chain for security
 * checks.
 *
 * @param ctx Approval context
 * @param bridge SafetyGuard bridge handle (NULL = bridge disabled)
 *
 * @ownership ctx: BORROW, bridge: BORROW (caller retains ownership)
 */
void tool_approval_set_safety_guard_bridge(tool_approval_ctx_t *ctx, safety_guard_bridge_t *bridge);

/**
 * @brief Get the approval context's Agent ID.
 *
 * @param ctx Approval context
 * @return Agent-ID string pointer (valid for ctx's lifetime), NULL if ctx is NULL
 *
 * @ownership ctx: BORROW; return: BORROW
 */
const char *tool_approval_get_agent_id(const tool_approval_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_APPROVAL_H */