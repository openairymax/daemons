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
#include "err_class.h"
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
    char *buf = dlen < sizeof(stackbuf) ? stackbuf : (char *)AIRY_MALLOC(dlen + 1);
    if (!buf)
        return;
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

/* L2 呈现层按 class 分诊（0.1.19 §3.7.2 N1）：只有 transport 才允许说
 * "服务不可达"；business/policy/config/internal 必须透传原始错误码与
 * 文案，避免把工具业务失败误导为守护进程离线。 */
static char *run_fail_text(int rc)
{
    char buf[512];
    const char *s;
    if (rc == 0)
        rc = AIRY_ERR_PROTOCOL;
    s = airy_err_str(rc);
    if (!s || !s[0])
        s = "unknown error";
    if (airy_err_class_of(rc) == AIRY_ERR_CLASS_TRANSPORT)
        snprintf(buf, sizeof(buf), "Error: tool service unreachable (%s, code: %d)", s, rc);
    else
        snprintf(buf, sizeof(buf), "Error: %s (code: %d)", s, rc);
    return AIRY_STRDUP(buf);
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
        AIRY_FREE(resp);
        *out_text = run_fail_text(rc);
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
     * "error","exit_code","error_code"}（与 cli_chat_tools.c 同源；兼容未解包
     * 信封——有 "result" 先下钻）。工具业务错误（success=0）一律透传原始
     * error 文案与 error_code，不再折叠为不可读的通用话术（缺陷 #2）。 */
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsObject(result))
        result = root;
    int tool_ok = 0;
    if (result) {
        cJSON *success = cJSON_GetObjectItem(result, "success");
        cJSON *output = cJSON_GetObjectItem(result, "output");
        cJSON *error = cJSON_GetObjectItem(result, "error");
        cJSON *ecode = cJSON_GetObjectItem(result, "error_code");
        tool_ok = cJSON_IsNumber(success) && success->valueint != 0;
        if (tool_ok) {
            text = AIRY_STRDUP(cJSON_IsString(output) && output->valuestring ? output->valuestring :
                                                                               "(no output)");
        } else {
            const char *e =
                cJSON_IsString(error) && error->valuestring ? error->valuestring : "execution failed";
            int code = cJSON_IsNumber(ecode) ? ecode->valueint : 0;
            char diag[768];
            airy_err_t ec = AIRY_ERR_UNKNOWN;
            if (airy_err_json_get(result, NULL, &ec, diag, sizeof(diag), NULL) == 0 && diag[0]) {
                e = diag;
                code = (int)ec;
            }
            char buf[1024];
            if (code != 0)
                snprintf(buf, sizeof(buf), "Error: %s (code: %d)", e, code);
            else
                snprintf(buf, sizeof(buf), "Error: %s", e);
            text = AIRY_STRDUP(buf);
        }
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

typedef struct {
    const agent_run_loop_args_t *args;
    agent_run_loop_result_t *out;
    cJSON *messages;
    cJSON *trace;
    agent_ledger_t lg;
    char llm_sock[AGENT_RUN_SOCK_BUF];
    const char *sess;
    const char *run_id;
    uint64_t seq;
    char *text;
    char *reason;
    int streak;
    int fused;
    char fuse[256];
} run_ctx_t;

/* 首条 system：工具路径锚点契约（缺陷 #4）声明 fs_* 与 shell_run 的相对
 * 路径以 tool_d 工作目录为基准、回执回显绝对路径；0.1.18 B1 轮次归属约束
 * 仅在携带历史时合并入同一条，防止模型把多轮历史当作同一篇待续写文本。 */
static void run_preamble(cJSON *msgs)
{
    static const char base[] =
        "【工具路径契约】文件类工具（fs_*）与 shell_run 的相对路径以工具"
        "服务（tool_d）的工作目录为解析基准；工具结果回执会回显解析后的"
        "绝对路径。";
    static const char boundary[] =
        "【轮次边界】本次请求携带了历史消息。历史消息仅供指代消解与背景"
        "理解；你的回答必须直接回应最后一条用户消息。若历史主题与最后"
        "一条用户消息不一致，以最后一条用户消息为准，禁止延续历史主题"
        "作答，禁止续写历史中未完成的回答。";
    cJSON *sys = cJSON_CreateObject();
    if (!sys)
        return;
    char content[1024];
    snprintf(content, sizeof(content), "%s%s", base,
             cJSON_GetArraySize(msgs) > 1 ? boundary : "");
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", content);
    cJSON_InsertItemInArray(msgs, 0, sys);
}

/* 初始消息集与台账入口锚点：history 非空则整体复制，否则以 prompt 构造
 * 首条 user 消息。入口记账取首条真实 user 消息——注入的 system 前置不算
 * 入口，否则被保护的是提示词样板而非用户诉求。 */
static int run_seed(run_ctx_t *r)
{
    const cJSON *h = r->args->history;
    if (h && cJSON_IsArray(h) && cJSON_GetArraySize(h) > 0)
        r->messages = cJSON_Duplicate(h, 1);
    if (!r->messages) {
        r->messages = cJSON_CreateArray();
        cJSON *m0 = r->messages ? cJSON_CreateObject() : NULL;
        if (!m0)
            return -1;
        cJSON_AddItemToArray(r->messages, m0);
        cJSON_AddStringToObject(m0, "role", "user");
        cJSON_AddStringToObject(m0, "content", r->args->prompt);
    }
    run_preamble(r->messages);
    cJSON *m = NULL;
    cJSON_ArrayForEach(m, r->messages) {
        cJSON *role = cJSON_GetObjectItem(m, "role");
        if (cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0) {
            agent_ledger_add(&r->lg, "user",
                             cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")), 0, m);
            break;
        }
    }
    return 0;
}

/* reasoning 跨轮累积：各轮以换行连接；扩容失败保留已有内容不阻断。 */
static void run_reason_add(run_ctx_t *r, const char *add)
{
    size_t old = r->reason ? strlen(r->reason) : 0, cap = old + strlen(add) + 2;
    char *np = (char *)AIRY_REALLOC(r->reason, cap);
    if (!np)
        return;
    r->reason = np;
    snprintf(np + old, cap - old, old > 0 ? "\n%s" : "%s", add);
}

/* 事件投递：agent_run_emit_event 自行处理无 sink / 建包失败时的载荷释放。 */
static void run_ev_send(run_ctx_t *r, const char *type, cJSON *ev)
{
    agent_run_emit_event(r->args->sink, &r->seq, r->run_id, r->sess, type, ev);
}

/* 单轮 LLM：建参 → 流式收流 → 取产物 → 回填 assistant 消息与台账。
 * 返回 1 终局回复（正文已移交 r->text）、0 有待执行工具、-1 失败。 */
static int run_llm_round(run_ctx_t *r, cJSON **tool_calls)
{
    *tool_calls = NULL;
    char *params = run_build_llm_params(r->args->model, r->messages);
    if (!params)
        return -1;

    agent_stream_t st;
    run_delta_ctx_t dctx = {r->args->sink, &r->seq, r->run_id, r->sess};
    agent_stream_sink_t ssink = {run_emit_delta, &dctx};
    agent_stream_init(&st, &ssink);

    uint64_t t0 = airy_time_ms();
    int lrc = daemon_rpc_call_stream(r->llm_sock, "complete_stream", params,
                                     agent_stream_on_chunk, &st, AGENT_RUN_LLM_TIMEOUT_MS);
    r->out->llm_ms += airy_time_ms() - t0;
    AIRY_FREE(params);
    agent_stream_finish(&st);

    agent_stream_result_t sr;
    agent_stream_take(&st, &sr);
    agent_stream_free(&st);
    if (lrc != AIRY_SUCCESS || sr.error_code != 0) {
        SVC_LOG_ERROR("agent.run: llm stream failed (rc=%d, ec=%d, msg=%s, session=%s)", lrc,
                      sr.error_code, sr.error_msg, r->sess ? r->sess : "?");
        run_stream_result_free(&sr);
        return -1;
    }

    r->out->tokens += sr.tokens;
    r->out->cost += sr.cost;
    if (sr.reason && sr.reason[0])
        run_reason_add(r, sr.reason);
    AIRY_FREE(sr.reason);

    /* 空数组与「无 tool_calls」同义（旧解析器同判）：本轮即终局回复。 */
    cJSON *calls = sr.tools;
    if (calls && cJSON_GetArraySize(calls) == 0) {
        cJSON_Delete(calls);
        calls = NULL;
    }
    cJSON *am = cJSON_CreateObject();
    cJSON_AddStringToObject(am, "role", "assistant");
    cJSON_AddStringToObject(am, "content", sr.text ? sr.text : "");
    if (calls)
        cJSON_AddItemToObject(am, "tool_calls", cJSON_Duplicate(calls, 1));
    cJSON_AddItemToArray(r->messages, am);
    agent_ledger_add(&r->lg, "assistant", sr.text ? sr.text : "", (size_t)sr.tokens, am);

    if (calls) {
        AIRY_FREE(sr.text);
        *tool_calls = calls;
        return 0;
    }
    r->text = sr.text;
    return 1;
}

/* 单条工具调用：tool_start → 执行 → 连败计数 → tool_end → 回填 messages /
 * 台账 / trace（后者供最终响应）。返回熔断是否触发。 */
static int run_tool_one(run_ctx_t *r, cJSON *tc)
{
    cJSON *fn = cJSON_GetObjectItem(tc, "function");
    cJSON *fname = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
    cJSON *fargs = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
    cJSON *tcid = cJSON_GetObjectItem(tc, "id");
    const char *tname = cJSON_IsString(fname) ? fname->valuestring : "";
    const char *targs = cJSON_IsString(fargs) ? fargs->valuestring : "{}";
    const char *tid = cJSON_IsString(tcid) ? tcid->valuestring : "";
    char buf[160];

    AIRY_STRNCPY_TERM(buf, targs, sizeof(buf));
    cJSON *ev = cJSON_CreateObject();
    if (ev) {
        cJSON_AddStringToObject(ev, AIRY_RS_K_TOOL, tname);
        cJSON_AddStringToObject(ev, AIRY_RS_K_TOOL_ID, tid);
        cJSON_AddStringToObject(ev, AIRY_RS_K_ARGS, buf);
    }
    run_ev_send(r, AIRY_RS_TYPE_TOOL_START, ev);

    char *res = NULL;
    uint64_t t0 = airy_time_ms();
    int erc = run_execute_tool(tname, targs, &res);
    r->out->tool_ms += airy_time_ms() - t0;

    /* C-1 连败计数：本次失败仍完整入账/推送后再判断熔断出口。 */
    if (run_fail_streak_bump(&r->streak, erc)) {
        r->fused = 1;
        snprintf(r->fuse, sizeof(r->fuse), "%s: %s", tname, res ? res : "");
    }

    AIRY_STRNCPY_TERM(buf, res ? res : "", sizeof(buf));
    ev = cJSON_CreateObject();
    if (ev) {
        cJSON_AddStringToObject(ev, AIRY_RS_K_TOOL_ID, tid);
        cJSON_AddStringToObject(ev, AIRY_RS_K_STATUS, erc == 0 ? "ok" : "error");
        cJSON_AddStringToObject(ev, AIRY_RS_K_RESULT_HASH, buf);
    }
    run_ev_send(r, AIRY_RS_TYPE_TOOL_END, ev);

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "tool");
    cJSON_AddStringToObject(msg, "tool_call_id", tid);
    cJSON_AddStringToObject(msg, "content", res ? res : "Tool execution failed");
    cJSON_AddItemToArray(r->messages, msg);
    agent_ledger_add(&r->lg, "tool_result", res, 0, msg);

    cJSON *tr = cJSON_CreateObject();
    cJSON_AddStringToObject(tr, "tool", tname);
    cJSON_AddStringToObject(tr, "arguments", targs);
    cJSON_AddStringToObject(tr, "result", res ? res : "");
    cJSON_AddNumberToObject(tr, "ok", erc == 0 ? 1 : 0);
    cJSON_AddItemToArray(r->trace, tr);
    AIRY_FREE(res);
    return r->fused;
}

/* 单轮推进：一轮 LLM 收流 + 本轮全部工具调用。返回 1 继续下一轮、0 终局
 * 回复已就绪（正文在 r->text）、-1 失败、熔断返回 AGENT_RUN_RC_TOOL_FUSE
 * （收尾文案写入 r->text）。 */
static int run_step(run_ctx_t *r)
{
    cJSON *calls = NULL;
    char msg[384];
    int st = run_llm_round(r, &calls);
    if (st < 0)
        return -1;
    if (st > 0)
        return 0;

    int n = cJSON_GetArraySize(calls);
    for (int k = 0; k < n && !r->fused; k++)
        run_tool_one(r, cJSON_GetArrayItem(calls, k));
    cJSON_Delete(calls);
    if (!r->fused)
        return 1;

    /* C-1 熔断终局：止损退出（不再消耗 LLM 轮次），原因经 text 回传，
     * engine 侧 error 事件与 response 复用。 */
    snprintf(msg, sizeof(msg),
             "Tool execution failed %d times in a row; further attempts stopped (%s)",
             r->streak, r->fuse);
    r->text = AIRY_STRDUP(msg);
    return AGENT_RUN_RC_TOOL_FUSE;
}

int agent_run_tool_loop(const agent_run_loop_args_t *args, agent_run_loop_result_t *out)
{
    run_ctx_t r = {0};
    int rc = -1;
    if (!args || !out)
        return -1;
    AIRY_MEMSET(out, 0, sizeof(*out));
    r.args = args;
    r.out = out;
    r.sess = args->session ? args->session->session_id : NULL;
    r.run_id = args->session ? args->session->run_id : NULL;
    snprintf(r.llm_sock, sizeof(r.llm_sock), "%s", airy_runtime_dir_socket("llm.sock"));
    agent_ledger_init(&r.lg, r.sess);

    r.trace = cJSON_CreateArray();
    if (!r.trace || run_seed(&r) != 0) {
        cJSON_Delete(r.messages);
        cJSON_Delete(r.trace);
        return -1;
    }

    for (int i = 0; i < AGENT_RUN_MAX_TOOL_LOOPS; i++) {
        if (agent_run_is_cancelled(args->session)) {
            SVC_LOG_INFO("agent.run: cancelled by user (session=%s)", r.sess ? r.sess : "?");
            rc = 1;
            break;
        }
        agent_ledger_fit(&r.lg, r.messages);

        int st = run_step(&r);
        if (st != 1) {
            rc = st;
            break;
        }
    }

    agent_ledger_free(&r.lg);
    cJSON_Delete(r.messages);
    /* 成功/熔断移交产物，失败路径就地释放；计量已直写 out。 */
    if (rc == 0 || rc == AGENT_RUN_RC_TOOL_FUSE) {
        out->trace = r.trace;
        out->text = r.text;
        out->reason = r.reason;
        return rc;
    }
    cJSON_Delete(r.trace);
    AIRY_FREE(r.text);
    AIRY_FREE(r.reason);
    return rc;
}
