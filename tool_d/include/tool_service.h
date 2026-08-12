/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_service.h
 * @brief Public tool service interface.
 */

#ifndef AIRY_RT_TOOL_SERVICE_H
#define AIRY_RT_TOOL_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct tool_service tool_service_t;

/**
 * @brief Tool access type (improvement 1 P1d: parallel-tool concurrency gating).
 *
 * READ tools are read-only with no side effects -> concurrency gate read
 * lock (multiple tools run in parallel); WRITE tools have side effects ->
 * concurrency gate write lock (mutually exclusive, serial execution).
 * The first enum entry is WRITE (=0), so zero-initialized defaults to
 * mutually-exclusive serial execution (safe default).
 */
typedef enum {
    TOOL_ACCESS_WRITE = 0,
    TOOL_ACCESS_READ = 1,
} tool_access_t;

/** @brief Tool parameter definition (JSON Schema-format string). */
typedef struct {
    const char *name;
    const char *schema;
    int required; /* Whether required (0=optional, 1=required). Consistent
                   * with the gateway tool schema's required array (SSoT):
                   * fs_list.path is optional (omitted -> list the current
                   * dir), fs_read/fs_write/shell_run are required. */
} tool_param_t;

/** @brief Tool metadata. */
typedef struct {
    char *id;
    char *name;
    char *description;
    char *executable;
    tool_param_t *params;
    size_t param_count;
    int timeout_sec;
    int cacheable;
    tool_access_t access;
    char *permission_rule;
} tool_metadata_t;

/** @brief Tool-execution request. */
typedef struct {
    const char *tool_id;
    const char *params_json;
    int stream;
    const char *agent_id;
    void *user_data;
} tool_execute_request_t;

/**
 * @brief Tool-execution failure tiers (improvement 3: Codex
 *        Fatal/RespondToModel/normal three-state).
 *
 * Upper layers (taskflow/work-hall/blueprint scheduling) decide task
 * semantics by tier:
 *   - FATAL             -> terminate the task (fail-closed), cascade-cancel
 *                          related executions
 *   - RESPOND_TO_MODEL  -> return the result to the upper layer, task keeps
 *                          running (start failure/approval denial, etc.)
 *   - NORMAL_FAIL       -> return wrapped success:false, task continues
 *                          (retry configurable)
 */
typedef enum {
    TOOL_RESULT_CLASS_SUCCESS = 0,
    TOOL_RESULT_CLASS_FATAL,
    TOOL_RESULT_CLASS_RESPOND_TO_MODEL,
    TOOL_RESULT_CLASS_NORMAL_FAIL,
} tool_result_class_t;

/** @brief Tool-execution result (non-streaming). */
typedef struct {
    int success;
    char *output;
    char *error;
    int exit_code;
    uint64_t duration_ms;
    tool_result_class_t failure_class;
} tool_result_t;

/**
 * @brief Streaming-output callback.
 * @param chunk  Output data chunk
 * @param is_stderr Whether it is error output
 * @param user_data User data
 */
typedef void (*tool_stream_callback_t)(const char *chunk, int is_stderr, void *user_data);


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