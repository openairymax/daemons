/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file executor.h
 * @brief Tool-executor interface.
 */

#ifndef TOOL_EXECUTOR_H
#define TOOL_EXECUTOR_H

#include "config.h"
#include "tool_approval.h"
#include "tool_interactive_approval.h"
#include "tool_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool_executor tool_executor_t;

typedef struct {
    int max_workers;
    int timeout_sec;
    char *workbench_type;
} tool_executor_config_t;

tool_executor_t *tool_executor_create(const tool_executor_config_t *cfg);
tool_executor_t *tool_executor_create_ex(const tool_executor_config_t *ecfg);
void tool_executor_destroy(tool_executor_t *exec);

/**
 * @brief Execute a tool.
 * @param exec Executor
 * @param meta Tool metadata
 * @param params_json Parameter JSON
 * @param agent_id Caller Agent ID (NULL/empty = fall back to approval-context default, "tool_d")
 * @param out_result Output result
 * @return 0 on success, other error codes
 */
int tool_executor_run(tool_executor_t *exec, const tool_metadata_t *meta, const char *params_json,
                      const char *agent_id, tool_result_t **out_result);

typedef void (*tool_execute_callback_t)(tool_result_t *result, void *user_data);
int tool_executor_run_async(tool_executor_t *exec, const tool_metadata_t *meta,
                            const char *params_json, const char *agent_id,
                            tool_execute_callback_t callback, void *user_data,
                            tool_result_t **out_result);


void tool_executor_set_approval_ctx(tool_executor_t *exec, tool_approval_ctx_t *approval_ctx);


/**
 * @brief Whether interactive approval is enabled.
 * @param exec Executor
 * @return true enabled, false disabled
 *
 * @ownership exec: BORROW
 */
bool tool_executor_interactive_enabled(tool_executor_t *exec);

/**
 * @brief List all pending approval requests (JSON array string).
 * @param exec Executor
 * @return JSON array string (AIRY_MALLOC, caller AIRY_FREEs), NULL on failure
 *
 * @ownership exec: BORROW; return: OWNER
 */
char *tool_executor_interactive_pending_list(tool_executor_t *exec);

/**
 * @brief Resolve a pending approval request by request_id.
 * @param exec Executor
 * @param request_id Request ID
 * @param decision Decision: "allow" / "always" / "deny"
 * @return 0 on success; AIRY_ERR_NOT_FOUND not found; AIRY_ERR_INVALID_PARAM bad args
 *
 * @ownership exec: BORROW
 */
int tool_executor_interactive_resolve(tool_executor_t *exec, const char *request_id,
                                      const char *decision);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_EXECUTOR_H */