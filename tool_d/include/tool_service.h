/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_service.h
 * @brief Public tool service interface.
 */

#ifndef AIRY_RT_TOOL_SERVICE_H
#define AIRY_RT_TOOL_SERVICE_H

#include "tool_service_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


tool_service_t *tool_service_create(const char *config_path);
void tool_service_destroy(tool_service_t *svc);


int tool_service_register(tool_service_t *svc, const tool_metadata_t *meta);
int tool_service_unregister(tool_service_t *svc, const char *tool_id);
tool_metadata_t *tool_service_get(tool_service_t *svc, const char *tool_id);
void tool_metadata_free(tool_metadata_t *meta);
char *tool_service_list(tool_service_t *svc);


int tool_service_execute(tool_service_t *svc, const tool_execute_request_t *req,
                         tool_result_t **out_result);

int tool_service_execute_stream(tool_service_t *svc, const tool_execute_request_t *req,
                                tool_stream_callback_t callback, void *callback_data,
                                tool_result_t **out_result);

void tool_result_free(tool_result_t *res);


/**
 * @brief Get tool-service runtime stats (L2 standard method tool.get_stats).
 * @param svc Tool-service instance
 * @return JSON string (AIRY_MALLOC, caller AIRY_FREEs), NULL on failure
 *
 * Returned fields: daemon, tools (registered tool count), exec_total (total
 * executions), exec_fail (failure count), exec_ms_total (cumulative ms),
 * avg_exec_ms.
 */
char *tool_service_get_stats(tool_service_t *svc);


/**
 * @brief List all pending approval requests (JSON array string).
 * @param svc Tool-service instance
 * @return JSON array string (AIRY_MALLOC, caller AIRY_FREEs), NULL on failure
 *
 * Each element: {request_id, tool, agent_id, params, created_at}
 */
char *tool_service_interactive_pending_list(tool_service_t *svc);

/**
 * @brief Resolve a pending approval request by request_id.
 * @param svc Tool-service instance
 * @param request_id Request ID
 * @param decision Decision: "allow" / "always" / "deny"
 * @return 0 on success; AIRY_ERR_NOT_FOUND not found; AIRY_ERR_INVALID_PARAM bad args
 */
int tool_service_interactive_resolve(tool_service_t *svc, const char *request_id,
                                     const char *decision);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_SERVICE_H */