// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_run_loop.c
 * @brief Agent run 工具循环（ReAct，M1-1a 引擎下沉）。
 *
 * 自 gateway_biz_llm.c 迁移：LLM complete_stream -> 增量正文即时上屏 +
 * tool_calls -> tool_d 执行 -> 回填上下文 -> 继续。工具 schema 单一权威经
 * commons 契约层（airy_tool_schema.h）获取；llm_d/tool_d 经
 * daemon_rpc_client 直连；RS 分帧解析由 agent_run_stream.c 承担。
 */

#include "agent_run_internal.h"

#include "airy_memory.h"
#include "airy_run_stream.h"
#include "airy_tool_schema.h"
#include "daemon_rpc_client.h"
#include "platform.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGENT_RUN_SOCK_BUF AIRY_PATH_MAX

/* 增量上屏的栈缓冲上限：常见逐字/逐片增量远小于此值，超限走堆。 */
#define RUN_DELTA_STACK 1024

/* 构建 llm_d complete_stream 请求（透传 tools 数组与常规生成参数）。
 * 不在此处写 max_tokens：输出上限的权威在配置面（model.yaml max_output），
 * 由 llm_d 的生成参数解析统一裁决；调用方写死一个数值会让该配置永久失效。 */
static char *run_build_llm_params(const char *model, const cJSON *messages)
{
    cJSON *params = cJSON_CreateObject();
    if (!params)
        return NULL;
    cJSON_AddStringToObject(params, "model", model);
    cJSON_AddItemToObject(params, "messages", cJSON_Duplicate(messages, 1));
    cJSON *tools = cJSON_Parse(AIRY_TOOLS_JSON_SOURCE);
    if (tools)
        cJSON_AddItemToObject(params, "tools", tools);
    cJSON_AddNumberToObject(params, "temperature", 0.7);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    return params_str;
}

/* token_delta 推送上下文（流式期间由解帧器逐增量回调）。 */
typedef struct {
    const agent_run_event_sink_t *sink;
    uint64_t *seq;
    const char *run_id;
    const char *sess;
} run_delta_ctx_t;

/* 真实增量即时上屏：载荷按解帧器给出的长度全量承载（不截断、不分片丢内容）；
 * 空增量不产生事件，保证帧数与解帧器回调次数一一对应。
 * dlen 是权威长度：不依赖缓冲区在 dlen 处已置 NUL（长度超过栈缓冲时走堆）。 */
static void run_emit_delta(const char *delta, size_t dlen, void *ud)
{
    run_delta_ctx_t *c = (run_delta_ctx_t *)ud;
    if (!c || !c->sink || !c->sink->emit || !delta || dlen == 0)
        return;

    char stackbuf[RUN_DELTA_STACK];
    char *buf = stackbuf;
    if (dlen >= sizeof(stackbuf)) {
        buf = (char *)AIRY_MALLOC(dlen + 1);
        if (!buf)
            return;
    }
    AIRY_MEMCPY(buf, delta, dlen);
    buf[dlen] = '\0';

    cJSON *td = cJSON_CreateObject();
    if (td) {
        cJSON_AddStringToObject(td, AIRY_RS_K_DELTA, buf);
        agent_run_emit_event(c->sink, c->seq, c->run_id, c->sess, AIRY_RS_TYPE_TOKEN_DELTA, td);
    }
    if (buf != stackbuf)
        AIRY_FREE(buf);
}

/* 释放一次流消费的产物（已接管字段调用方先行摘除）。 */
static void run_stream_result_free(agent_stream_result_t *sr)
{
    AIRY_FREE(sr->text);
    AIRY_FREE(sr->reason);
    if (sr->tools)
        cJSON_Delete(sr->tools);
    AIRY_MEMSET(sr, 0, sizeof(*sr));
}

/* 单个工具执行：tool_d.execute_tool（结果文本化，失败转 Error: 前缀）。 */
static int run_execute_tool(const char *name, const char *args_json, char **out_text)
{
    *out_text = NULL;
    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON_AddStringToObject(params, "tool_id", name);
    cJSON *pargs = cJSON_Parse(args_json && args_json[0] ? args_json : "{}");
    if (!pargs)
        pargs = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "params", pargs);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    char sock[AGENT_RUN_SOCK_BUF];
    snprintf(sock, sizeof(sock), "%s", airy_runtime_dir_socket("tool.sock"));
    char *resp = NULL;
    int rc = daemon_rpc_call(sock, "execute_tool", params_str, &resp, AGENT_RUN_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        *out_text = AIRY_STRDUP("Tool service unreachable");
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root) {
        *out_text = AIRY_STRDUP("Tool service returned invalid response");
        return -1;
    }
    char *text = NULL;
    /* daemon_rpc_call 已解包 JSON-RPC 外层：resp 即 {"success","output",
     * "error","exit_code"}（与 cli_chat_tools.c 2026-08-16 修复同源；
     * 兼容未解包信封——有 "result" 先下钻）。 */
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsObject(result))
        result = root;
    cJSON *err = cJSON_GetObjectItem(root, "error");
    int tool_ok = 0;
    if (!err && result) {
        cJSON *success = cJSON_GetObjectItem(result, "success");
        cJSON *output = cJSON_GetObjectItem(result, "output");
        cJSON *error = cJSON_GetObjectItem(result, "error");
        tool_ok = cJSON_IsNumber(success) && success->valueint != 0;
        if (tool_ok) {
            text = AIRY_STRDUP(cJSON_IsString(output) && output->valuestring ? output->valuestring :
                                                                               "(no output)");
        } else {
            const char *e =
                cJSON_IsString(error) && error->valuestring ? error->valuestring : "execution failed";
            size_t elen = strlen(e) + 8;
            text = (char *)AIRY_MALLOC(elen);
            if (text)
                snprintf(text, elen, "Error: %s", e);
        }
    } else if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        const char *m = cJSON_IsString(msg) && msg->valuestring ? msg->valuestring : "unknown";
        size_t elen = strlen(m) + 8;
        text = (char *)AIRY_MALLOC(elen);
        if (text)
            snprintf(text, elen, "Error: %s", m);
    }
    if (!text)
        text = AIRY_STRDUP("Tool execution returned no result");
    *out_text = text;
    cJSON_Delete(root);
    return tool_ok ? 0 : -1;
}

/* 连败计数更新（C-1 熔断）：成功清零、失败累加；返回是否达到阈值。 */
static int run_fail_streak_bump(int *streak, int tool_rc)
{
    *streak = (tool_rc == 0) ? 0 : *streak + 1;
    return *streak >= AGENT_RUN_TOOL_FAIL_LIMIT;
}

int agent_run_tool_loop(const agent_run_loop_args_t *args, agent_run_loop_result_t *out)
{
    if (!args || !out)
        return -1;
    AIRY_MEMSET(out, 0, sizeof(*out));

    const char *prompt = args->prompt;
    const cJSON *history = args->history;
    const char *model = args->model;
    const agent_run_session_t *session = args->session;
    const agent_run_event_sink_t *sink = args->sink;

    cJSON *messages = NULL;
    if (history && cJSON_IsArray(history) && cJSON_GetArraySize(history) > 0) {
        messages = cJSON_Duplicate(history, 1);
    }
    if (!messages) {
        messages = cJSON_CreateArray();
        cJSON *msg0 = cJSON_CreateObject();
        cJSON_AddStringToObject(msg0, "role", "user");
        cJSON_AddStringToObject(msg0, "content", prompt);
        cJSON_AddItemToArray(messages, msg0);
    }
    /* 0.1.18 B1 轮次归属约束：请求携带多条消息（真实历史）时头插 system，
     * 声明历史仅供指代消解——修复模型把多轮历史视为同一篇待续写文本、
     * 对旧题续答的上下文串轮。单条消息无需注入（无串轮源）。 */
    if (cJSON_GetArraySize(messages) > 1) {
        cJSON *sys = cJSON_CreateObject();
        if (sys) {
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content",
                "【轮次边界】本次请求携带了历史消息。历史消息仅供指代消解与背景"
                "理解；你的回答必须直接回应最后一条用户消息。若历史主题与最后"
                "一条用户消息不一致，以最后一条用户消息为准，禁止延续历史主题"
                "作答，禁止续写历史中未完成的回答。");
            cJSON_InsertItemInArray(messages, 0, sys);
        }
    }
    cJSON *tool_trace = cJSON_CreateArray();
    if (!messages || !tool_trace) {
        if (messages)
            cJSON_Delete(messages);
        if (tool_trace)
            cJSON_Delete(tool_trace);
        return -1;
    }

    char *final_text = NULL;
    char *reasoning_acc = NULL;
    uint64_t total_tokens = 0;
    double total_cost = 0.0;
    uint64_t llm_ms = 0;
    uint64_t tool_ms = 0;
    int rc = -1;
    uint64_t seq = 0;
    const char *sess = session ? session->session_id : NULL;

    /* C-1 连败熔断状态：计数跨工具/跨轮累计（环境故障换工具照样失败），
     * 触发时捕获末次工具名与错误文本（tool_calls 释放后仍需可引用）。 */
    int fail_streak = 0;
    int fused = 0;
    char fuse_tool[64] = "";
    char fuse_err[192] = "";

    agent_ledger_t lg;
    agent_ledger_init(&lg, sess);

    char llm_sock[AGENT_RUN_SOCK_BUF];
    snprintf(llm_sock, sizeof(llm_sock), "%s", airy_runtime_dir_socket("llm.sock"));

    /* 台账入口记账：首条 user 消息（history 为空时即 prompt）。 */
    if (lg.enabled) {
        cJSON *m0 = cJSON_GetArrayItem(messages, 0);
        cJSON *c0 = m0 ? cJSON_GetObjectItem(m0, "content") : NULL;
        if (cJSON_IsString(c0))
            agent_ledger_add(&lg, "user", c0->valuestring, 0, m0);
    }

    for (int loops = 0; loops < AGENT_RUN_MAX_TOOL_LOOPS; loops++) {
        if (agent_run_is_cancelled(session)) {
            SVC_LOG_INFO("agent.run: cancelled by user (session=%s)",
                         session ? session->session_id : "?");
            rc = 1;
            break;
        }

        agent_ledger_fit(&lg, messages);

        char *llm_params_str = run_build_llm_params(model, messages);
        if (!llm_params_str)
            break;

        /* complete_stream：正文增量在收流过程中即时上屏（run_emit_delta），
         * 控制帧由解帧器累积为 tool_calls / reasoning / usage / error。 */
        agent_stream_t stream;
        run_delta_ctx_t dctx = {sink, &seq, session ? session->run_id : NULL, sess};
        agent_stream_sink_t ssink = {run_emit_delta, &dctx};
        agent_stream_init(&stream, &ssink);

        uint64_t llm_t0 = airy_time_ms();
        int lrc = daemon_rpc_call_stream(llm_sock, "complete_stream", llm_params_str,
                                         agent_stream_on_chunk, &stream, AGENT_RUN_LLM_TIMEOUT_MS);
        llm_ms += airy_time_ms() - llm_t0;
        AIRY_FREE(llm_params_str);
        agent_stream_finish(&stream);

        agent_stream_result_t sr;
        agent_stream_take(&stream, &sr);
        agent_stream_free(&stream);

        if (lrc != AIRY_SUCCESS) {
            SVC_LOG_ERROR("agent.run: llm complete_stream failed (rc=%d, session=%s)", lrc,
                          sess ? sess : "?");
            run_stream_result_free(&sr);
            break;
        }
        if (sr.error_code != 0) {
            SVC_LOG_ERROR("agent.run: llm stream error %d (%s)", sr.error_code, sr.error_msg);
            run_stream_result_free(&sr);
            break;
        }

        cJSON *tool_calls = sr.tools;
        char *text = sr.text;
        char *reasoning = sr.reason;
        uint64_t tokens = sr.tokens;
        sr.tools = NULL;
        sr.text = NULL;
        sr.reason = NULL;
        /* 空数组与「无 tool_calls」同义（旧解析器同判）：本轮即终局回复。 */
        if (tool_calls && cJSON_GetArraySize(tool_calls) == 0) {
            cJSON_Delete(tool_calls);
            tool_calls = NULL;
        }
        total_tokens += tokens;
        total_cost += sr.cost;
        if (reasoning && reasoning[0]) {
            size_t old = reasoning_acc ? strlen(reasoning_acc) : 0;
            size_t add = strlen(reasoning);
            char *np = (char *)AIRY_REALLOC(reasoning_acc, old + add + 2);
            if (np) {
                reasoning_acc = np;
                if (old > 0)
                    reasoning_acc[old++] = '\n';
                AIRY_MEMCPY(reasoning_acc + old, reasoning, add);
                reasoning_acc[old + add] = '\0';
            }
        }
        AIRY_FREE(reasoning);

        cJSON *assistant_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(assistant_msg, "role", "assistant");
        cJSON_AddStringToObject(assistant_msg, "content", text ? text : "");
        if (tool_calls)
            cJSON_AddItemToObject(assistant_msg, "tool_calls", cJSON_Duplicate(tool_calls, 1));
        cJSON_AddItemToArray(messages, assistant_msg);
        agent_ledger_add(&lg, "assistant", text ? text : "", (size_t)tokens, assistant_msg);

        if (!tool_calls) {
            final_text = text;
            text = NULL;
            rc = 0;
            break;
        }
        AIRY_FREE(text);

        int tc_count = cJSON_GetArraySize(tool_calls);
        for (int i = 0; i < tc_count; i++) {
            cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *fn_name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            cJSON *fn_args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
            cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
            const char *tname = cJSON_IsString(fn_name) ? fn_name->valuestring : "";
            const char *targs = cJSON_IsString(fn_args) ? fn_args->valuestring : "{}";
            const char *tid = cJSON_IsString(tc_id) ? tc_id->valuestring : "";

            /* tool_start 事件（run_stream 流式推送） */
            if (sink && sink->emit) {
                cJSON *ts = cJSON_CreateObject();
                if (ts) {
                    cJSON_AddStringToObject(ts, AIRY_RS_K_TOOL, tname);
                    cJSON_AddStringToObject(ts, AIRY_RS_K_TOOL_ID, tid);
                    char abuf[160];
                    AIRY_STRNCPY_TERM(abuf, targs, sizeof(abuf));
                    cJSON_AddStringToObject(ts, AIRY_RS_K_ARGS, abuf);
                    agent_run_emit_event(sink, &seq, session ? session->run_id : NULL, sess, AIRY_RS_TYPE_TOOL_START, ts);
                }
            }

            char *result_text = NULL;
            uint64_t tool_t0 = airy_time_ms();
            int erc = run_execute_tool(tname, targs, &result_text);
            tool_ms += airy_time_ms() - tool_t0;

            /* C-1 连败计数：本次失败仍完整入账/推送后再判断熔断出口。 */
            if (run_fail_streak_bump(&fail_streak, erc)) {
                fused = 1;
                AIRY_STRNCPY_TERM(fuse_tool, tname, sizeof(fuse_tool));
                AIRY_STRNCPY_TERM(fuse_err, result_text ? result_text : "", sizeof(fuse_err));
            }

            /* tool_end 事件（run_stream 流式推送） */
            if (sink && sink->emit) {
                cJSON *te = cJSON_CreateObject();
                if (te) {
                    cJSON_AddStringToObject(te, AIRY_RS_K_TOOL_ID, tid);
                    cJSON_AddStringToObject(te, AIRY_RS_K_STATUS, erc == 0 ? "ok" : "error");
                    char rbuf[160];
                    AIRY_STRNCPY_TERM(rbuf, result_text ? result_text : "", sizeof(rbuf));
                    cJSON_AddStringToObject(te, AIRY_RS_K_RESULT_HASH, rbuf);
                    agent_run_emit_event(sink, &seq, session ? session->run_id : NULL, sess, AIRY_RS_TYPE_TOOL_END, te);
                }
            }

            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", tid);
            cJSON_AddStringToObject(tool_msg, "content",
                                    result_text ? result_text : "Tool execution failed");
            cJSON_AddItemToArray(messages, tool_msg);
            agent_ledger_add(&lg, "tool_result", result_text ? result_text : NULL, 0, tool_msg);

            cJSON *tr = cJSON_CreateObject();
            cJSON_AddStringToObject(tr, "tool", tname);
            cJSON_AddStringToObject(tr, "arguments", targs);
            cJSON_AddStringToObject(tr, "result", result_text ? result_text : "");
            cJSON_AddNumberToObject(tr, "ok", erc == 0 ? 1 : 0);
            cJSON_AddItemToArray(tool_trace, tr);

            if (result_text)
                AIRY_FREE(result_text);

            if (fused)
                break;
        }

        cJSON_Delete(tool_calls);

        if (fused) {
            /* C-1 熔断终局：止损退出（不再消耗 LLM 轮次），原因经
             * final_text 回传，engine 侧 error 事件与 response 复用。 */
            char fuse_msg[512];
            snprintf(fuse_msg, sizeof(fuse_msg),
                     "Tool execution failed %d times in a row; further attempts stopped "
                     "(last tool=%s, error=%s)",
                     fail_streak, fuse_tool, fuse_err);
            final_text = AIRY_STRDUP(fuse_msg);
            rc = AGENT_RUN_RC_TOOL_FUSE;
            break;
        }
    }

    agent_ledger_free(&lg);
    cJSON_Delete(messages);

    if (rc == 0 || rc == AGENT_RUN_RC_TOOL_FUSE) {
        out->trace = tool_trace;
        out->text = final_text;
        out->reason = reasoning_acc;
        out->tokens = total_tokens;
        out->cost = total_cost;
        out->llm_ms = llm_ms;
        out->tool_ms = tool_ms;
    } else {
        if (final_text)
            AIRY_FREE(final_text);
        cJSON_Delete(tool_trace);
        AIRY_FREE(reasoning_acc);
    }
    return rc;
}
