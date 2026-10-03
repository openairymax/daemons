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
#include "svc_agent_d.h"

#include "airy_memory.h"
#include "airy_run_stream.h"
#include "daemon_platform_ext.h"
#include "daemon_rpc_client.h"
#include "jsonrpc_helpers.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <string.h>

/* ---- run_stream 流式事件推送（M1-1d 协议先行） ---- */

/* sink->emit 回调：把事件信封序列化为单行 JSON 帧推送 socket（\n 收尾，
 * 与 llm_d complete_stream 的流式 framing 一致；EOF 由连接关闭表示）。 */
typedef struct {
    airy_sock_t fd;
    int failed;
} rs_sink_ctx_t;

static void rs_sink_emit(const char *type, cJSON *env, void *ud)
{
    rs_sink_ctx_t *ctx = (rs_sink_ctx_t *)ud;
    (void)type;
    if (!ctx || ctx->failed || !env)
        return;
    char *json = cJSON_PrintUnformatted(env);
    if (!json)
        return;
    size_t jl = strlen(json);
    char *frame = (char *)AIRY_MALLOC(jl + 2);
    if (!frame) {
        AIRY_FREE(json);
        return;
    }
    AIRY_MEMCPY(frame, json, jl);
    frame[jl] = '\n';
    frame[jl + 1] = '\0';
    AIRY_FREE(json);
    if (airy_sock_send(ctx->fd, frame, jl + 1) < 0)
        ctx->failed = 1;
    AIRY_FREE(frame);
}

/* agent.run / agent.run_stream 公共入参：JSON params -> 引擎七参。 */
typedef struct {
    const char *prompt;
    const char *model;
    cJSON *history;    /* 借用 messages 数组节点（可 NULL） */
    const char *gccp_answers;
    cJSON *agent_spec; /* 借用 params.agent 节点（可 NULL） */
    const char *agent_file;
    const char *session_id;
} run_args_t;

/* 取对象字符串字段；非字符串或空串视同缺省，返回 NULL。 */
static const char *json_str(cJSON *o, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    return (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) ? v->valuestring : NULL;
}

/* 解析公共入参；缺参时下发 JSON-RPC 错误并返回 -1。 */
static int parse_run_args(cJSON *params, int id, airy_sock_t fd, run_args_t *out)
{
    if (!params) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Missing params", id);
        return -1;
    }

    cJSON *p = cJSON_GetObjectItem(params, "prompt");
    const char *prompt = cJSON_IsString(p) ? p->valuestring : NULL;
    cJSON *messages = cJSON_GetObjectItem(params, "messages");
    cJSON *m0 = (cJSON_IsArray(messages) && cJSON_GetArraySize(messages) > 0) ?
                    cJSON_GetArrayItem(messages, 0) :
                    NULL;
    if (!prompt && m0)
        prompt = json_str(m0, "content");
    if (!prompt || !*prompt) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Invalid params: missing prompt", id);
        return -1;
    }

    out->prompt = prompt;
    out->model = json_str(params, "model");
    out->history = (cJSON_IsArray(messages) && cJSON_GetArraySize(messages) > 0) ? messages : NULL;
    out->gccp_answers = json_str(params, "gccp_answers");
    out->agent_spec = cJSON_GetObjectItem(params, "agent");
    out->agent_file = json_str(params, "agent_file");
    out->session_id = json_str(params, "session_id");
    return 0;
}

/* agent.run_stream 请求处理：解析 params -> 引擎（带事件 sink 流式推送）。 */
static void handle_run_stream(cJSON *params, int id, airy_sock_t client_fd)
{
    run_args_t a;
    if (parse_run_args(params, id, client_fd, &a) != 0)
        return;

    rs_sink_ctx_t sink_ctx = {client_fd, 0};
    agent_run_event_sink_t sink = {rs_sink_emit, &sink_ctx};

    cJSON *result = NULL;
    int rc = agent_run_execute(a.prompt, a.model, a.history, a.gccp_answers, a.agent_spec,
                               a.agent_file, a.session_id, &sink, &result);
    if (result)
        cJSON_Delete(result);
    (void)rc;
    /* 流式帧已逐条推送；连接关闭即 EOF，客户端 daemon_rpc_call_stream 据此收尾。 */
}

void m_run_stream(cJSON *params, int id, void *user_data)
{
    handle_run_stream(params, id, *(airy_sock_t *)user_data);
}

/* agent.run 请求处理：解析 params -> 引擎 -> 组装响应。 */
static void handle_run(cJSON *params, int id, airy_sock_t client_fd)
{
    run_args_t a;
    if (parse_run_args(params, id, client_fd, &a) != 0)
        return;

    cJSON *result = NULL;
    int rc = agent_run_execute(a.prompt, a.model, a.history, a.gccp_answers, a.agent_spec,
                               a.agent_file, a.session_id, NULL, &result);
    if (rc == 1) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Request cancelled by user", id);
        cJSON_Delete(result);
        return;
    }
    if (rc != 0 || !result) {
        /* 失败原因可判读：引擎把原因写入 response（如工具连败熔断说明）。 */
        const char *detail = NULL;
        if (result) {
            cJSON *resp = cJSON_GetObjectItem(result, "response");
            if (cJSON_IsString(resp) && resp->valuestring && resp->valuestring[0])
                detail = resp->valuestring;
        }
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           detail ? detail :
                                     "agent.run failed: tool loop exhausted or LLM service error",
                           id);
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

void m_run(cJSON *params, int id, void *user_data)
{
    handle_run(params, id, *(airy_sock_t *)user_data);
}

void m_run_cancel(cJSON *params, int id, void *user_data)
{
    handle_cancel(params, id, *(airy_sock_t *)user_data);
}
