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
/**
 * @brief Replace an already-registered tool record in place.
 *
 * Used by the in-session tool workshop (tool_create) to redefine a script
 * tool it created earlier. Built-in entries (executable has the "builtin:"
 * prefix) are never replaceable.
 *
 * @return 0 on success; AIRY_ERR_UNKNOWN when the tool does not exist;
 *         AIRY_ERR_PERMISSION_DENIED when the target is built-in.
 */
int tool_service_replace(tool_service_t *svc, const tool_metadata_t *meta);
tool_metadata_t *tool_service_get(tool_service_t *svc, const char *tool_id);
void tool_metadata_free(tool_metadata_t *meta);
/**
 * @brief Release the members owned by a metadata record and zero it.
 *
 * Use this for a record living inside a larger allocation (e.g. an
 * embedded pool job); tool_metadata_free() additionally frees the
 * record itself.
 */
void tool_metadata_clear(tool_metadata_t *meta);
/**
 * @brief Deep-copy src into caller-provided storage dst (which needs no
 *        prior initialisation).
 * @return 0 on success, -1 on allocation failure (dst stays clearable).
 */
int tool_metadata_copy(tool_metadata_t *dst, const tool_metadata_t *src);
/**
 * @brief Deep-copy a tool metadata record (all strings and the param array).
 * @return heap copy owned by the caller (release with tool_metadata_free),
 *         NULL on allocation failure.
 */
tool_metadata_t *tool_metadata_clone(const tool_metadata_t *src);
char *tool_service_list(tool_service_t *svc);
/* C-16：include_denied=1 时同时返回不可用条目及 denied_reason（目录协商用）。
 * include_denied=0 与 tool_service_list 同集，仅多带 available/denied_reason
 * 字段（恒为 true/""）。 */
char *tool_service_list_ex(tool_service_t *svc, int include_denied);


int tool_service_execute(tool_service_t *svc, const tool_execute_request_t *req,
                         tool_result_t **out_result);

/**
 * @brief N-9 审批干跑：只判「这次调用会不会走到交互等待」，不裁决、不阻塞。
 * @param svc Tool-service instance
 * @param tool_id Tool id (BORROW)
 * @param params_json Parameter JSON (BORROW)
 * @param agent_id Caller agent id (BORROW, may be NULL)
 * @param allow_dangerous D-9 explicit danger bypass flag
 * @return JSON string {"needed":bool,"timeout_ms":n} (AIRY_MALLOC, caller
 *         AIRY_FREEs); NULL when the tool is unknown or probe fails —
 *         callers must then proceed without an approval window (the real
 *         execution still passes the fail-closed gate).
 */
char *tool_service_approval_probe(tool_service_t *svc, const char *tool_id,
                                  const char *params_json, const char *agent_id,
                                  int allow_dangerous);

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
char *svc_int_pending(tool_service_t *svc);

/**
 * @brief Resolve a pending approval request by request_id.
 * @param svc Tool-service instance
 * @param request_id Request ID
 * @param decision Decision: "allow" / "always" / "deny"
 * @return 0 on success; AIRY_ERR_NOT_FOUND not found; AIRY_ERR_INVALID_PARAM bad args
 */
int svc_int_resolve(tool_service_t *svc, const char *request_id,
                    const char *decision);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_SERVICE_H */
