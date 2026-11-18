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

#include "run/agent_run_engine.h"
#include "run/agent_run_internal.h"
#include "agent_d_internal.h"
#include "svc_agent_d.h"

#include "airy_memory.h"
#include "airy_run_stream.h"
#include "daemon_platform_ext.h"
#include "daemon_rpc_client.h"
#include "error.h"
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
    int rounds;               /* params.rounds（缺省 0，交由引擎按策略解析） */
    const char *resume_token; /* params.resume_token（可 NULL） */
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
    const char *resume = json_str(params, "resume_token");
    cJSON *messages = cJSON_GetObjectItem(params, "messages");
    cJSON *m0 = (cJSON_IsArray(messages) && cJSON_GetArraySize(messages) > 0) ?
                    cJSON_GetArrayItem(messages, 0) :
                    NULL;
    if (!prompt && m0)
        prompt = json_str(m0, "content");
    /* 续跑请求可以只带令牌：原始诉求与已完成步骤都在令牌里，客户端无需
     * 重发 messages（C-14 的「不得整轮重放」）。 */
    if ((!prompt || !*prompt) && !resume) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS,
                           "Invalid params: missing prompt or resume_token", id);
        return -1;
    }

    cJSON *rounds = cJSON_GetObjectItem(params, "rounds");
    out->prompt = prompt;
    out->model = json_str(params, "model");
    out->history = (cJSON_IsArray(messages) && cJSON_GetArraySize(messages) > 0) ? messages : NULL;
    out->gccp_answers = json_str(params, "gccp_answers");
    out->agent_spec = cJSON_GetObjectItem(params, "agent");
    out->agent_file = json_str(params, "agent_file");
    out->session_id = json_str(params, "session_id");
    out->rounds = cJSON_IsNumber(rounds) ? rounds->valueint : 0;
    out->resume_token = resume;
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
                               a.agent_file, a.session_id, a.rounds, a.resume_token, NULL,
                               &sink, &result);
    if (result)
        cJSON_Delete(result);
    (void)rc;
    /* 流式帧已逐条推送；连接关闭即 EOF，客户端 daemon_rpc_call_stream 据此收尾。 */
}

DAEMON_RPC_SHELL(run_stream, handle_run_stream)

/* D-10：失败面唯一出口。把引擎 result 的 failure 载荷挂到 error.data，
 * 客户端无需解析文案即可知道原因与下一步；无载荷时才退回 err_detail。
 * 协议层保持 error（以 result 返回会让调用方按成功路径继续）。 */
static void run_send_fail(airy_sock_t fd, int id, const char *detail, const cJSON *failure)
{
    const char *msg = (detail && detail[0]) ? detail : "agent.run failed";
    cJSON *data = failure ? cJSON_Duplicate((cJSON *)failure, 1) : cJSON_CreateObject();
    cJSON *rid = cJSON_CreateNumber(id);
    char *resp = NULL;

    if (data && !failure && detail && detail[0])
        cJSON_AddStringToObject(data, AIRY_RS_K_DETAIL, detail);
    if (rid)
        resp = jsonrpc_create_error_response(rid, JSONRPC_INTERNAL_ERROR, msg, data);
    cJSON_Delete(rid);
    if (!resp) {
        /* 构包失败时 data 已随 error_response 释放，统一走无 data 兜底。 */
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, msg, id);
        return;
    }
    (void)jsonrpc_route_response(fd, resp, strlen(resp));
    AIRY_FREE(resp);
}

/* agent.run 请求处理：解析 params -> 引擎 -> 组装响应。 */
static void handle_run(cJSON *params, int id, airy_sock_t client_fd)
{
    run_args_t a;
    if (parse_run_args(params, id, client_fd, &a) != 0)
        return;

    cJSON *result = NULL;
    int rc = agent_run_execute(a.prompt, a.model, a.history, a.gccp_answers, a.agent_spec,
                               a.agent_file, a.session_id, a.rounds, a.resume_token, NULL,
                               NULL, &result);
    if (rc == 1) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Request cancelled by user", id);
        cJSON_Delete(result);
        return;
    }
    /* 预算耗尽不是失败：result 携带 status=paused、error_code 与 resume_token，
     * 客户端据此三选（继续/放弃/看失败步骤），因此按成功帧下发。 */
    if ((rc != 0 && rc != AIRY_EBUDGET) || !result) {
        /* 失败原因可判读：引擎把原因写入 response（如工具连败熔断说明），
         * D-10 另带结构化 failure，客户端无需再解析文案。 */
        const char *detail = NULL;
        const cJSON *failure = NULL;
        if (result) {
            cJSON *resp = cJSON_GetObjectItem(result, "response");
            if (cJSON_IsString(resp) && resp->valuestring && resp->valuestring[0])
                detail = resp->valuestring;
            failure = cJSON_GetObjectItem(result, AIRY_RS_K_FAILURE);
        }
        run_send_fail(client_fd, id, detail, failure);
        if (result)
            cJSON_Delete(result);
        return;
    }

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ---- task.submit / task.submit_stream：统一任务编排入口（C-15） ---- */

/* task 入参：goal 与 agent.run 的 prompt 同义，另带模式声明与 GCCP 材料。 */
typedef struct {
    const char *goal;
    const char *model;
    cJSON *history;            /* 借用 messages 数组节点（可 NULL） */
    const char *gccp_answers;  /* 借用字符串，或指向 gccp_owned */
    char *gccp_owned;          /* 对象答案序列化后的临时串（自持有） */
    const char *session_id;
    const char *resume_token;
    const char *declared_mode; /* "chat"/"task"/"auto"/NULL */
    const char *success_criteria;
    const char *client;
    int rounds;
    bool gccp_confirmed;
} task_args_t;

static int parse_task_args(cJSON *params, int id, airy_sock_t fd, task_args_t *out)
{
    AIRY_MEMSET(out, 0, sizeof(*out));
    if (!params) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS, "Missing params", id);
        return -1;
    }

    cJSON *p = cJSON_GetObjectItem(params, "goal");
    const char *goal = cJSON_IsString(p) ? p->valuestring : NULL;
    cJSON *messages = cJSON_GetObjectItem(params, "messages");
    cJSON *m0 = (cJSON_IsArray(messages) && cJSON_GetArraySize(messages) > 0)
                    ? cJSON_GetArrayItem(messages, 0)
                    : NULL;
    if (!goal && m0)
        goal = json_str(m0, "content");
    if (!goal)
        goal = json_str(params, "prompt");
    const char *resume = json_str(params, "resume_token");
    if ((!goal || !*goal) && !resume) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INVALID_PARAMS,
                           "Invalid params: missing goal or resume_token", id);
        return -1;
    }

    cJSON *rounds = cJSON_GetObjectItem(params, "rounds");
    if (!cJSON_IsNumber(rounds)) {
        /* 线协议以 budget{rounds,tokens,seconds} 为主形态；顶层 rounds 保留
         * 兼容旧调用面。 */
        cJSON *budget = cJSON_GetObjectItem(params, "budget");
        if (cJSON_IsObject(budget))
            rounds = cJSON_GetObjectItem(budget, "rounds");
    }
    cJSON *conf = cJSON_GetObjectItem(params, "gccp_confirmed");
    out->goal = goal;
    out->model = json_str(params, "model");
    out->history = m0 ? messages : NULL;
    out->session_id = json_str(params, "session_id");
    out->resume_token = resume;
    /* "auto" 与缺省同义：交给服务端规则裁决，客户端不自行推断模式。 */
    out->declared_mode = json_str(params, "mode");
    if (out->declared_mode && strcmp(out->declared_mode, AGENT_TASK_MODE_AUTO) == 0)
        out->declared_mode = NULL;
    out->success_criteria = json_str(params, "success_criteria");
    out->client = json_str(params, "client");
    out->rounds = cJSON_IsNumber(rounds) ? rounds->valueint : 0;
    out->gccp_confirmed = cJSON_IsBool(conf) && cJSON_IsTrue(conf);

    /* gccp_answers 兼容字符串与对象两种线形态：字符串供 think_d 直接消费，
     * 对象需序列化后才能作为引擎入参。 */
    cJSON *ans = cJSON_GetObjectItem(params, "gccp_answers");
    if (cJSON_IsString(ans) && ans->valuestring && ans->valuestring[0]) {
        out->gccp_answers = ans->valuestring;
    } else if (cJSON_IsObject(ans)) {
        out->gccp_owned = cJSON_PrintUnformatted(ans);
        out->gccp_answers = out->gccp_owned;
    }
    return 0;
}

/* 同步/流式共用主流程：裁决模式 -> GCCP 门禁 -> 引擎执行。
 * 未确认的 task 在门禁处直接返回 needs_intent，绝不触达引擎与工具。 */
static void task_submit_common(cJSON *params, int id, airy_sock_t fd,
                               const agent_run_event_sink_t *sink)
{
    task_args_t a;
    if (parse_task_args(params, id, fd, &a) != 0)
        return;

    agent_run_task_meta_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    agent_task_gen_id("task", meta.task_id, sizeof(meta.task_id));
    agent_task_gen_id("plan", meta.plan_id, sizeof(meta.plan_id));
    meta.success_criteria = a.success_criteria;
    meta.client = a.client;
    meta.budget_rounds = a.rounds;
    meta.gccp_confirmed = a.gccp_confirmed;

    if (agent_task_decide_mode(a.goal, a.declared_mode, meta.mode, sizeof(meta.mode),
                               meta.mode_reason, sizeof(meta.mode_reason)) != 0) {
        AIRY_FREE(a.gccp_owned);
        if (!sink)
            JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR,
                               "task.submit failed: mode decision", id);
        return;
    }

    /* GCCP 门禁：显式 confirmed 与按槽位齐备的答案二者其一即可放行。 */
    cJSON *ans_raw = cJSON_GetObjectItem(params, "gccp_answers");
    const cJSON *answers = cJSON_IsObject(ans_raw) ? ans_raw : NULL;
    cJSON *answers_parsed = NULL;
    if (!answers && cJSON_IsString(ans_raw) && ans_raw->valuestring && ans_raw->valuestring[0]) {
        answers_parsed = cJSON_Parse(ans_raw->valuestring);
        answers = answers_parsed;
    }
    agent_task_missing_slot_t missing[AGENT_TASK_MISSING_MAX];
    size_t missing_count = 0;
    bool confirmed = meta.gccp_confirmed;
    (void)agent_task_gccp_check(answers, &confirmed, missing, AGENT_TASK_MISSING_MAX,
                                &missing_count);
    meta.gccp_confirmed = confirmed;
    cJSON_Delete(answers_parsed);

    /* 门禁（唯一判定实现）：关闭时零引擎 / 零工具调用，直接返回
     * needs_intent（gate:lab：`-p` 未确认不得产生任何工具调用事件）。 */
    if (!agent_task_gate_allows(meta.mode, meta.gccp_confirmed)) {
        cJSON *ni = agent_task_build_needs_intent(&meta, missing, missing_count);
        AIRY_FREE(a.gccp_owned);
        if (!ni) {
            if (!sink)
                JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR,
                                   "task.submit failed: needs_intent build", id);
            return;
        }
        if (!sink) {
            JSONRPC_SEND_SUCCESS(fd, ni, id);
            return;
        }
        /* 流式无 JSON-RPC 成功帧：把 needs_intent 作为 task_state 帧下发，
         * 客户端按 state 渲染补全表单（TUI C-15-06 消费）。 */
        cJSON_AddStringToObject(ni, AIRY_RS_K_STATE, AGENT_TASK_STATUS_NEEDS_INTENT);
        uint64_t seq = 0;
        agent_run_emit_event(sink, &seq, meta.task_id, "", AIRY_RS_TYPE_TASK_STATE, ni);
        return;
    }

    cJSON *result = NULL;
    int rc = agent_run_execute(a.goal, a.model, a.history, a.gccp_answers, NULL, NULL,
                               a.session_id, a.rounds, a.resume_token, &meta, sink, &result);
    AIRY_FREE(a.gccp_owned);
    if (sink) {
        if (result)
            cJSON_Delete(result);
        return;
    }
    if (rc == 1) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Request cancelled by user", id);
        cJSON_Delete(result);
        return;
    }
    if ((rc != 0 && rc != AIRY_EBUDGET) || !result) {
        const char *detail = NULL;
        const cJSON *failure = NULL;
        if (result) {
            cJSON *resp = cJSON_GetObjectItem(result, "response");
            if (cJSON_IsString(resp) && resp->valuestring && resp->valuestring[0])
                detail = resp->valuestring;
            failure = cJSON_GetObjectItem(result, AIRY_RS_K_FAILURE);
        }
        run_send_fail(fd, id, detail, failure);
        if (result)
            cJSON_Delete(result);
        return;
    }
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_task_submit(cJSON *params, int id, airy_sock_t client_fd)
{
    task_submit_common(params, id, client_fd, NULL);
}

static void handle_task_submit_stream(cJSON *params, int id, airy_sock_t client_fd)
{
    rs_sink_ctx_t sink_ctx = {client_fd, 0};
    agent_run_event_sink_t sink = {rs_sink_emit, &sink_ctx};
    task_submit_common(params, id, client_fd, &sink);
}

/* agent.cancel 请求处理：按 session_id 置位取消标志。 */
static void handle_cancel(cJSON *params, int id, airy_sock_t client_fd)
{
    JSONRPC_REQUIRE(params, client_fd, "Missing params", id);
    cJSON *sid = cJSON_GetObjectItem(params, "session_id");
    JSONRPC_REQUIRE(cJSON_IsString(sid) && sid->valuestring && *sid->valuestring, client_fd,
                    "Invalid params: missing session_id", id);
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

DAEMON_RPC_SHELL(run, handle_run)
DAEMON_RPC_SHELL(run_cancel, handle_cancel)
/* task.* 对外方法由 gateway 映射为无点方法名（方法分派按首个 '.' 截断）。 */
DAEMON_RPC_SHELL(task_submit, handle_task_submit)
DAEMON_RPC_SHELL(task_submit_stream, handle_task_submit_stream)
