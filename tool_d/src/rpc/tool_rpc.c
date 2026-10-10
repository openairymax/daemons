/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_rpc.c
 * @brief tool.* RPC 方法域：工具注册 / 列举 / 查询 / 执行 /
 *        健康检查 / 服务统计 / 交互审批（pending / approve）。
 *
 * 由 main.c 按单一职责拆分（0.1.19 gen5 装配）：method_fn 薄壳 m_* 把
 * 机制层注入的 &client_fd 翻译为协议无关的 handler（cJSON*, int,
 * airy_sock_t）。L2 协议别名 list_tools->list、execute_tool->execute。
 */

#include "airy_memory.h"
#include "error.h"
#include "svc_tool_d.h"
#include "tool_d_internal.h"

#include "jsonrpc_helpers.h"
#include "param_validator.h"
#include "svc_logger.h"

#include <time.h>

static void handle_register(cJSON *params, int id, airy_sock_t fd);
static void handle_list(int id, airy_sock_t fd);
static void handle_get(cJSON *params, int id, airy_sock_t fd);
static void handle_execute(cJSON *params, int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);
static void handle_stats(int id, airy_sock_t fd);
static void handle_pending(int id, airy_sock_t fd);
static void handle_approve(cJSON *params, int id, airy_sock_t fd);

DAEMON_RPC_SHELL(register, handle_register)
DAEMON_RPC_SHELL0(list_tools, handle_list)
DAEMON_RPC_SHELL(get_tool, handle_get)
DAEMON_RPC_SHELL(execute_tool, handle_execute)

/* L2 协议别名：tool.execute == execute_tool；tool.list == list_tools
 * （02-l2-service-protocol.md）。 */
DAEMON_RPC_SHELL(execute, handle_execute)
DAEMON_RPC_SHELL0(list, handle_list)
DAEMON_RPC_SHELL0(health_check, handle_health_check)
DAEMON_RPC_SHELL0(get_stats, handle_stats)
DAEMON_RPC_SHELL0(pending, handle_pending)
DAEMON_RPC_SHELL(approve, handle_approve)

static void handle_register(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *tool = jsonrpc_get_object_param(params, "tool");
    if (!tool) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing tool object", id);
        return;
    }

    tool_metadata_t meta = {0};
    const char *tid = get_string_field(tool, "id", NULL);
    const char *tname = get_string_field(tool, "name", NULL);
    const char *texec = get_string_field(tool, "executable", NULL);

    if (!tid || !tname || !texec) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Invalid tool fields: id, name, executable required", id);
        return;
    }

    meta.id = (char *)tid;
    meta.name = (char *)tname;
    meta.executable = (char *)texec;

    meta.description = (char *)get_string_field(tool, "description", NULL);
    meta.timeout_sec = get_int_field(tool, "timeout_sec", 0);
    meta.cacheable = get_bool_field(tool, "cacheable", false);
    meta.permission_rule = (char *)get_string_field(tool, "permission_rule", NULL);

    cJSON *params_arr = cJSON_GetObjectItem(tool, "params");
    if (cJSON_IsArray(params_arr)) {
        size_t cnt = cJSON_GetArraySize(params_arr);
        tool_param_t *p = (tool_param_t *)AIRY_CALLOC(cnt, sizeof(tool_param_t));
        if (p) {
            for (size_t i = 0; i < cnt; ++i) {
                cJSON *item = cJSON_GetArrayItem(params_arr, i);
                cJSON *pname = cJSON_GetObjectItem(item, "name");
                cJSON *pschema = cJSON_GetObjectItem(item, "schema");
                if (cJSON_IsString(pname))
                    p[i].name = pname->valuestring;
                if (cJSON_IsString(pschema))
                    p[i].schema = pschema->valuestring;
            }
            meta.params = p;
            meta.param_count = cnt;
        }
    }

    int ret = tool_service_register(g_service, &meta);
    AIRY_FREE((void *)meta.params);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Register failed", id);
        SVC_LOG_ERROR("Failed to register tool: %s (error=%d)", meta.id, ret);
    } else {
        JSONRPC_SEND_SUCCESS(client_fd, NULL, id);
        SVC_LOG_INFO("Tool registered successfully: %s", meta.id);
    }
}

static void handle_list(int id, airy_sock_t client_fd)
{
    char *list_json = tool_service_list(g_service);
    if (!list_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "List failed", id);
        return;
    }

    CJSON_PARSE_GUARD(result, list_json, {
        AIRY_FREE(list_json);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid JSON from list", id);
        return;
    });
    AIRY_FREE(list_json);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    result = NULL;
}

static void handle_get(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *tid = get_string_field(params, "tool_id", NULL);
    if (!tid) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing tool_id", id);
        return;
    }

    tool_metadata_t *meta = tool_service_get(g_service, tid);
    if (!meta) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Tool not found", id);
        return;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", meta->id);
    cJSON_AddStringToObject(obj, "name", meta->name);
    cJSON_AddStringToObject(obj, "executable", meta->executable);
    if (meta->description)
        cJSON_AddStringToObject(obj, "description", meta->description);
    cJSON_AddNumberToObject(obj, "timeout_sec", meta->timeout_sec);
    cJSON_AddBoolToObject(obj, "cacheable", meta->cacheable);
    if (meta->permission_rule)
        cJSON_AddStringToObject(obj, "permission_rule", meta->permission_rule);

    if (meta->param_count > 0) {
        cJSON *params_arr = cJSON_CreateArray();
        for (size_t i = 0; i < meta->param_count; ++i) {
            cJSON *pobj = cJSON_CreateObject();
            cJSON_AddStringToObject(pobj, "name", meta->params[i].name);
            cJSON_AddStringToObject(pobj, "schema", meta->params[i].schema);
            cJSON_AddItemToArray(params_arr, pobj);
        }
        cJSON_AddItemToObject(obj, "params", params_arr);
    }

    JSONRPC_SEND_SUCCESS(client_fd, obj, id);
    tool_metadata_free(meta);
}

static void handle_execute(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *tid = get_string_field(params, "tool_id", NULL);
    cJSON *jparams = jsonrpc_get_object_param(params, "params");
    /* P0 interactive approval: optionally pass through the caller's agent_id
     * (e.g. an agent child process's coding_v1) so the ACL judges by the real
     * subject; when absent, fall back to the "tool_d" default (existing
     * behavior unchanged). */
    const char *agent_id = get_string_field(params, "agent_id", NULL);

    if (!tid || !jparams) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Invalid execute params: tool_id and params required", id);
        return;
    }

    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Tool service not ready", id);
        return;
    }

    char *params_json = cJSON_PrintUnformatted(jparams);
    if (!params_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "JSON serialization failed", id);
        return;
    }

    tool_execute_request_t req = {.tool_id = tid,
                                  .params_json = params_json,
                                  .stream = 0,
                                  .agent_id = agent_id};

    tool_result_t *res = NULL;
    int ret = tool_service_execute(g_service, &req, &res);
    AIRY_FREE((void *)params_json);

    /* Error layering: a JSON-RPC error envelope is reserved for protocol /
     * transport failures only. Tool business outcomes (including "tool ran
     * and failed", i.e. res->success == 0, and request-level rejections such
     * as a denied interactive approval) MUST travel as a result envelope so
     * the caller can surface the original error text and code instead of
     * misreporting the daemon as unreachable. */
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        if (res) {
            tool_result_free(res);
        }
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }

    if (res) {
        cJSON_AddNumberToObject(result, "success", res->success);
        if (res->output)
            cJSON_AddStringToObject(result, "output", res->output);
        if (res->error)
            cJSON_AddStringToObject(result, "error", res->error);
        cJSON_AddNumberToObject(result, "exit_code", res->exit_code);
        cJSON_AddNumberToObject(result, "error_code", ret);
        if (ret != AIRY_OK)
            SVC_LOG_ERROR("Tool execution failed: %s (error=%d)", tid, ret);
        tool_result_free(res);
    } else {
        const char *emsg = airy_err_str(ret);
        if (!emsg || !emsg[0])
            emsg = "Execution failed";
        cJSON_AddNumberToObject(result, "success", 0);
        cJSON_AddStringToObject(result, "error", emsg);
        cJSON_AddNumberToObject(result, "exit_code", -1);
        cJSON_AddNumberToObject(result, "error_code", ret);
        SVC_LOG_ERROR("Tool execution rejected: %s (error=%d)", tid, ret);
    }

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_health_check(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "tool_d");
    cJSON_AddBoolToObject(result, "healthy", g_service != NULL);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_stats(int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Tool service not ready", id);
        return;
    }
    char *stats_json = tool_service_get_stats(g_service);
    if (!stats_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Failed to collect stats", id);
        return;
    }

    cJSON *result = cJSON_Parse(stats_json);
    AIRY_FREE(stats_json);
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Stats serialization failed", id);
        return;
    }

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_pending(int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Tool service not ready", id);
        return;
    }
    char *pending_json = svc_int_pending(g_service);
    cJSON *arr = pending_json ? cJSON_Parse(pending_json) : NULL;
    AIRY_FREE(pending_json);
    if (!arr) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Failed to list pending approvals",
                           id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "pending", arr);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_approve(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Tool service not ready", id);
        return;
    }
    const char *request_id = get_string_field(params, "request_id", NULL);
    const char *decision = get_string_field(params, "decision", NULL);
    if (!request_id || !decision) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "request_id and decision required",
                           id);
        return;
    }
    int ret = svc_int_resolve(g_service, request_id, decision);
    if (ret == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "resolved", true);
        cJSON_AddStringToObject(result, "request_id", request_id);
        cJSON_AddStringToObject(result, "decision", decision);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_INFO("P0: tool.approve resolved request_id=%s decision=%s", request_id, decision);
    } else if (ret == AIRY_ERR_NOT_FOUND) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Approval request not found", id);
    } else if (ret == AIRY_ERR_INVALID_PARAM) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Invalid decision (allow/always/deny)", id);
    } else {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Failed to resolve approval", id);
    }
}
