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

#include "daemon_replies.h"
#include "err_class.h"
#include "jsonrpc_helpers.h"
#include "param_validator.h"
#include "svc_logger.h"

#include "core/tool_contract.h"

static void handle_register(cJSON *params, int id, airy_sock_t fd);
static void handle_list(cJSON *params, int id, airy_sock_t fd);
static void handle_get(cJSON *params, int id, airy_sock_t fd);
static void handle_execute(cJSON *params, int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);
static void handle_stats(int id, airy_sock_t fd);
static void handle_pending(int id, airy_sock_t fd);
static void handle_approve(cJSON *params, int id, airy_sock_t fd);
static void handle_submit(cJSON *params, int id, airy_sock_t fd);
static void handle_poll(cJSON *params, int id, airy_sock_t fd);
static void handle_cancel(cJSON *params, int id, airy_sock_t fd);

/* g_service 未就绪是全部 tool.* 方法共有的失败前置，收敛为单一出口。 */
#define TOOLRPC_REQUIRE_SERVICE(fd, id)                                        \
    do {                                                                       \
        if (!g_service) {                                                      \
            JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR,                     \
                               "Tool service not ready", id);                  \
            return;                                                            \
        }                                                                      \
    } while (0)

/* 作业存储未就绪与 g_service 同属「能力缺失」，统一返回内部错误。 */
#define TOOLRPC_REQUIRE_JOBS(fd, id)                                           \
    do {                                                                       \
        if (!g_jobs) {                                                         \
            JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR,                     \
                               "Job store not ready", id);                     \
            return;                                                            \
        }                                                                      \
    } while (0)

DAEMON_RPC_SHELL(register, handle_register)
DAEMON_RPC_SHELL(list_tools, handle_list)
DAEMON_RPC_SHELL(get_tool, handle_get)
DAEMON_RPC_SHELL(execute_tool, handle_execute)

/* L2 协议别名：tool.execute == execute_tool；tool.list == list_tools
 * （02-l2-service-protocol.md）。 */
DAEMON_RPC_SHELL(execute, handle_execute)
DAEMON_RPC_SHELL(list, handle_list)
DAEMON_RPC_SHELL0(health_check, handle_health_check)
DAEMON_RPC_SHELL0(get_stats, handle_stats)
DAEMON_RPC_SHELL0(pending, handle_pending)
DAEMON_RPC_SHELL(approve, handle_approve)
DAEMON_RPC_SHELL(submit, handle_submit)
DAEMON_RPC_SHELL(poll, handle_poll)
DAEMON_RPC_SHELL(cancel, handle_cancel)

static void handle_register(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *tool = jsonrpc_get_object_param(params, "tool");
    JSONRPC_REQUIRE(tool, client_fd, "Missing tool object", id);

    tool_metadata_t meta = {0};
    const char *tid = get_string_field(tool, "id", NULL);
    const char *tname = get_string_field(tool, "name", NULL);
    const char *texec = get_string_field(tool, "executable", NULL);

    JSONRPC_REQUIRE(tid && tname && texec, client_fd,
                    "Invalid tool fields: id, name, executable required", id);

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

static void handle_list(cJSON *params, int id, airy_sock_t client_fd)
{
    /* C-16：目录协商。include_denied 缺省/非法一律按 0 处理（只回可见集），
     * 保证旧客户端行为不变。 */
    cJSON *inc = params ? cJSON_GetObjectItemCaseSensitive(params, "include_denied") : NULL;
    int include_denied = cJSON_IsTrue(inc) || (cJSON_IsNumber(inc) && inc->valueint != 0);
    char *list_json = tool_service_list_ex(g_service, include_denied);
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
    JSONRPC_REQUIRE(tid, client_fd, "Missing tool_id", id);

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

/* L0 产生层：失败必须携带 {class, code, diag} 三元组，禁止裸返回；ctx
 * 记录发起工具，供上层排障定位（0.1.19 §3.7.2 N1）。policy 类额外附带
 * sandbox_scope（允许根，与 ToolContract v1 的 list_tools 同键同形），
 * 使调用方无需猜测被拒路径的合法落点（N3）。 */
static void put_policy_scope(cJSON *ctx)
{
    tool_contract_env_t env;
    if (tool_contract_env(&env) == 0)
        tool_scope_put(ctx, env.path_base, 1);
}

static void put_tool_err(cJSON *result, airy_err_t code, const char *diag, const char *tid)
{
    airy_err_class_t cls = airy_err_class_of(code);
    cJSON *ctx = cJSON_CreateObject();
    if (ctx && tid)
        cJSON_AddStringToObject(ctx, "tool", tid);
    if (ctx && cls == AIRY_ERR_CLASS_POLICY)
        put_policy_scope(ctx);
    airy_err_json_put(result, cls, code, diag, ctx);
    cJSON_Delete(ctx);
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

    JSONRPC_REQUIRE(tid && jparams, client_fd,
                    "Invalid execute params: tool_id and params required", id);

    TOOLRPC_REQUIRE_SERVICE(client_fd, id);

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
     * and failed", i.e. err_code != 0, and request-level rejections such
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
        int ok = tool_result_ok(res);
        /* 业务失败（err_code 非 0）没有外层 errno 承载时，用结果自身的
         * err_code 提升为 error_code；此前一律填 0，调用方只能看到失败。 */
        airy_err_t ecode = ok ? ret
                              : (ret != AIRY_OK ? ret
                                                : (res->err_code != 0 ? res->err_code
                                                                      : AIRY_ERR_GENERIC_FAIL));
        cJSON_AddNumberToObject(result, "success", ok ? 1 : 0);
        if (res->output)
            cJSON_AddStringToObject(result, "output", res->output);
        if (res->error)
            cJSON_AddStringToObject(result, "error", res->error);
        cJSON_AddNumberToObject(result, "exit_code", res->exit_code);
        cJSON_AddNumberToObject(result, "err_code", (double)res->err_code);
        cJSON_AddNumberToObject(result, "error_code", (double)ecode);
        if (!ok) {
            put_tool_err(result, ecode, res->error ? res->error : airy_err_str(ecode), tid);
            SVC_LOG_ERROR("Tool execution failed: %s (err_code=%d)", tid, (int)res->err_code);
        }
        tool_result_free(res);
    } else {
        const char *emsg = airy_err_str(ret);
        if (!emsg || !emsg[0])
            emsg = "Execution failed";
        cJSON_AddNumberToObject(result, "success", 0);
        cJSON_AddStringToObject(result, "error", emsg);
        cJSON_AddNumberToObject(result, "exit_code", -1);
        cJSON_AddNumberToObject(result, "error_code", ret);
        put_tool_err(result, ret, emsg, tid);
        SVC_LOG_ERROR("Tool execution rejected: %s (error=%d)", tid, ret);
    }

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_health_check(int id, airy_sock_t client_fd)
{
    reply_health(client_fd, "tool_d", g_service != NULL, id);
}

static void handle_stats(int id, airy_sock_t client_fd)
{
    TOOLRPC_REQUIRE_SERVICE(client_fd, id);
    reply_stats(client_fd, tool_service_get_stats(g_service), id);
}

static void handle_pending(int id, airy_sock_t client_fd)
{
    TOOLRPC_REQUIRE_SERVICE(client_fd, id);
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

static void handle_submit(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *tid = get_string_field(params, "tool_id", NULL);
    cJSON *jparams = jsonrpc_get_object_param(params, "params");
    const char *agent_id = get_string_field(params, "agent_id", NULL);
    int timeout = get_int_field(params, "timeout_sec", 0);
    char job_id[TOOL_JOB_ID_MAX];
    char *params_json;
    int ret;

    JSONRPC_REQUIRE(tid && jparams, client_fd,
                    "Invalid submit params: tool_id and params required", id);
    TOOLRPC_REQUIRE_SERVICE(client_fd, id);
    TOOLRPC_REQUIRE_JOBS(client_fd, id);

    params_json = cJSON_PrintUnformatted(jparams);
    if (!params_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "JSON serialization failed", id);
        return;
    }

    tool_execute_request_t req = {.tool_id = tid,
                                  .params_json = params_json,
                                  .stream = 0,
                                  .agent_id = agent_id,
                                  .timeout_sec = timeout};
    ret = tooljob_submit(g_jobs, &req, timeout, job_id, sizeof(job_id));
    AIRY_FREE((void *)params_json);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           ret == AIRY_ERR_BUSY ? "Too many running jobs" : airy_err_str(ret), id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(result, "job_id", job_id);
    cJSON_AddStringToObject(result, "state", "running");
    cJSON_AddNumberToObject(result, "timeout_sec", tooljob_clamp(timeout));
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_poll(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *job_id = get_string_field(params, "job_id", NULL);
    tool_job_info_t info;
    int ret;

    JSONRPC_REQUIRE(job_id, client_fd, "Missing job_id", id);
    TOOLRPC_REQUIRE_JOBS(client_fd, id);

    ret = tooljob_poll(g_jobs, job_id, &info);
    if (ret != 0) {
        if (ret == AIRY_ERR_NOT_FOUND)
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Job not found", id);
        else
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, airy_err_str(ret), id);
        return;
    }

    cJSON *result = tooljob_json(&info);
    tooljob_rel(&info);
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_cancel(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *job_id = get_string_field(params, "job_id", NULL);
    int ret;

    JSONRPC_REQUIRE(job_id, client_fd, "Missing job_id", id);
    TOOLRPC_REQUIRE_JOBS(client_fd, id);

    ret = tooljob_cancel(g_jobs, job_id);
    if (ret == AIRY_ERR_NOT_FOUND) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Job not found", id);
        return;
    }
    if (ret == AIRY_ERR_STATE_ERROR) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Job already finished", id);
        return;
    }
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, airy_err_str(ret), id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(result, "job_id", job_id);
    cJSON_AddStringToObject(result, "state", "cancelled");
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_approve(cJSON *params, int id, airy_sock_t client_fd)
{
    TOOLRPC_REQUIRE_SERVICE(client_fd, id);
    const char *request_id = get_string_field(params, "request_id", NULL);
    const char *decision = get_string_field(params, "decision", NULL);
    JSONRPC_REQUIRE(request_id && decision, client_fd, "request_id and decision required", id);
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
