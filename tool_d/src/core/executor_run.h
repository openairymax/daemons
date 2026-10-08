/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file executor_run.h
 * @brief Tool-executor synchronous and asynchronous execution.
 */

#ifndef TOOL_EXECUTOR_RUN_H
#define TOOL_EXECUTOR_RUN_H

#include "tool_service_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool_executor tool_executor_t;

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

#ifdef __cplusplus
}
#endif

#endif /* TOOL_EXECUTOR_RUN_H */
