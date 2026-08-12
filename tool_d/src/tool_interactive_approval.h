/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_interactive_approval.h
 * @brief P0: tool-level interactive permission approval (Claude Code-style
 *        permission prompt).
 *
 * When AIRY_TOOL_APPROVAL_MODE=interactive, tool executions rejected by
 * static approval no longer fail closed with EPERM directly; instead they
 * enqueue a pending approval request and block waiting for an external
 * tool.approve decision (allow/always/deny), with a default timeout of
 * AIRY_TOOL_APPROVAL_TIMEOUT_MS (120000ms).
 */

#ifndef AIRY_RT_TOOL_INTERACTIVE_APPROVAL_H
#define AIRY_RT_TOOL_INTERACTIVE_APPROVAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Interactive-approval decision result. */
typedef enum {
    AIRY_APPROVAL_DENIED = 0,
    AIRY_APPROVAL_ALLOWED,
    AIRY_APPROVAL_ALWAYS
} airy_approval_outcome_t;


typedef struct interactive_approval interactive_approval_t;

/**
 * @brief Create the interactive-approval manager (reads env vars to decide
 *        whether to enable).
 * @return Manager handle, NULL on failure
 *
 * Environment variables:
 * - AIRY_TOOL_APPROVAL_MODE: enables interactive approval when "interactive"
 *   (default static)
 * - AIRY_TOOL_APPROVAL_TIMEOUT_MS: blocking-wait timeout (default 120000ms)
 *
 * @ownership return: OWNER
 */
interactive_approval_t *interactive_approval_create(void);

/**
 * @brief Destroy the interactive-approval manager.
 * @param mgr Manager
 *
 * @ownership mgr: TRANSFER
 */
void interactive_approval_destroy(interactive_approval_t *mgr);

/**
 * @brief Whether interactive approval is enabled (AIRY_TOOL_APPROVAL_MODE=interactive).
 * @param mgr Manager
 * @return true enabled, false disabled
 *
 * @ownership mgr: BORROW
 */
bool interactive_approval_is_enabled(const interactive_approval_t *mgr);

/**
 * @brief Enqueue a pending approval request and block for the external
 *        decision.
 *
 * The calling thread blocks until tool.approve decides or times out. The
 * timeout follows AIRY_TOOL_APPROVAL_TIMEOUT_MS; on timeout the decision
 * is AIRY_APPROVAL_DENIED.
 *
 * @param mgr Manager
 * @param tool Tool name
 * @param agent_id Caller Agent ID
 * @param params_json Tool-parameter JSON (copied)
 * @param out_outcome Output decision result (allow/always/deny/timeout)
 * @return Request-ID string (AIRY_MALLOC, caller AIRY_FREEs), NULL on failure
 *
 * @ownership params_json: BORROW; return: OWNER
 */
char *interactive_approval_block(interactive_approval_t *mgr, const char *tool,
                                 const char *agent_id, const char *params_json,
                                 airy_approval_outcome_t *out_outcome);

/**
 * @brief Resolve a pending request by request_id.
 * @param mgr Manager
 * @param request_id Request ID
 * @param decision Decision: "allow" / "always" / "deny"
 * @return 0 on success; AIRY_ERR_NOT_FOUND request not found; AIRY_ERR_INVALID_PARAM bad args
 *
 * @ownership mgr: BORROW
 */
int interactive_approval_resolve(interactive_approval_t *mgr, const char *request_id,
                                 const char *decision);

/**
 * @brief List all pending approval requests (JSON array string).
 * @param mgr Manager
 * @return JSON array string (AIRY_MALLOC, caller AIRY_FREEs), NULL on failure
 *
 * Each element: {request_id, tool, agent_id, params, created_at}
 *
 * @ownership return: OWNER
 */
char *interactive_approval_pending_list_json(interactive_approval_t *mgr);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_INTERACTIVE_APPROVAL_H */