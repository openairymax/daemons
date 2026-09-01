// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_run_rpc.c
 * @brief agent.run / agent.cancel RPC 适配层（M1-1a 引擎下沉）。
 *
 * agent.run 由 gateway 迁入 agent_d 的进程内引擎后，外部调用方
 * （CLI/SDK/经 gateway 转发）以 JSON-RPC `agent.run` / `agent.cancel`
 * 访问。本文件把请求参数解析为引擎入参，并把引擎结果组装为与旧
 * gateway 一致的 JSON-RPC 响应（session_id/response/tokens/cost/
 * tool_trace/thinking 契约不变），保证 CLI/TUI/SDK 零改动迁移。
 */

#include "agent_run_engine.h"
#include "agent_d_internal.h"

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "daemon_rpc_client.h"
#include "jsonrpc_helpers.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <string.h>

/* agent.run 请求处理：解析 params -> 引擎 -> 组装响应。 */
static void handle_run(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!params) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing params", id);
        return;
    }

    const char *prompt = NULL;
    cJSON *p = cJSON_GetObjectItem(params, "prompt");
    if (cJSON_IsString(p)) {
        prompt = p->valuestring;
    } else {
        cJSON *messages = cJSON_GetObjectItem(params, "messages");
        cJSON *m0 = (messages && cJSON_GetArraySize(messages) > 0) ?
                        cJSON_GetArrayItem(messages, 0) :
                        NULL;
        cJSON *c = m0 ? cJSON_GetObjectItem(m0, "content") : NULL;
        if (cJSON_IsString(c))
            prompt = c->valuestring;
    }
    if (!prompt || !*prompt) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Invalid params: missing prompt", id);
        return;
    }

    const char *model = NULL;
    cJSON *m = cJSON_GetObjectItem(params, "model");
    if (cJSON_IsString(m) && m->valuestring && m->valuestring[0])
        model = m->valuestring;

    cJSON *history = cJSON_GetObjectItem(params, "messages");
    if (!cJSON_IsArray(history) || cJSON_GetArraySize(history) == 0)
        history = NULL;

    const char *gccp_answers = NULL;
    cJSON *ga = cJSON_GetObjectItem(params, "gccp_answers");
    if (cJSON_IsString(ga) && ga->valuestring && ga->valuestring[0])
        gccp_answers = ga->valuestring;

    cJSON *agent_spec = cJSON_GetObjectItem(params, "agent");

    const char *agent_file = NULL;
    cJSON *af = cJSON_GetObjectItem(params, "agent_file");
    if (cJSON_IsString(af) && af->valuestring && af->valuestring[0])
        agent_file = af->valuestring;

    const char *session_id = NULL;
    cJSON *sid = cJSON_GetObjectItem(params, "session_id");
    if (cJSON_IsString(sid) && sid->valuestring && sid->valuestring[0])
        session_id = sid->valuestring;

    cJSON *result = NULL;
    int rc = agent_run_execute(prompt, model, history, gccp_answers, agent_spec, agent_file,
                               session_id, &result);
    if (rc == 1) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Request cancelled by user", id);
        cJSON_Delete(result);
        return;
    }
    if (rc != 0 || !result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "agent.run failed: tool loop exhausted or LLM service error", id);
        if (result)
            cJSON_Delete(result);
        return;
    }

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* agent.cancel 请求处理：按 session_id 置位取消标志。 */
static void handle_cancel(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!params) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing params", id);
        return;
    }
    cJSON *sid = cJSON_GetObjectItem(params, "session_id");
    if (!cJSON_IsString(sid) || !sid->valuestring || !*sid->valuestring) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Invalid params: missing session_id",
                           id);
        return;
    }
    int rc = agent_run_cancel_by_session(sid->valuestring);
    if (rc != AIRY_SUCCESS) {
        SVC_LOG_DEBUG("agent.cancel miss (session=%s, 请求已完成或不存在)", sid->valuestring);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND,
                           "No active request with given session_id", id);
        return;
    }
    SVC_LOG_INFO("agent.cancel set (session=%s)", sid->valuestring);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "cancelling");
    cJSON_AddStringToObject(result, "session_id", sid->valuestring);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

void on_run_method(cJSON *params, int id, void *user_data)
{
    handle_run(params, id, *(airy_sock_t *)user_data);
}

void on_run_cancel_method(cJSON *params, int id, void *user_data)
{
    handle_cancel(params, id, *(airy_sock_t *)user_data);
}
