// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_run_exec.c
 * @brief agent.run 主流程编排（会话开启 → 分支派发 → 结果组装 → 流式收尾）。
 *
 * 本文件只做一次 run 的阶段编排：会话注册与模型解析、run_start 发帧、
 * spec 解析、编排/主对话分支、决策链事件、result 组装与 run_stream 收尾；
 * 并承接 think_d / mem_d / hall 三个依赖域的薄适配。
 * 会话注册表与 spawn+invoke 编排在 agent_run_engine.c，ReAct 工具循环在
 * agent_run_loop.c，流式解复用在 agent_run_stream.c。
 *
 * 所有权约定：cJSON 与字符串在阶段间移动时同步置空源字段，收尾统一由
 * run_free() 释放；result 挂载的节点在挂载后立即移交（源置空）。
 */

#include "agent_run_engine.h"
#include "agent_run_internal.h"

#include "airy_memory.h"
#include "airy_run_stream.h"
#include "daemon_rpc_client.h"
#include "error.h"
#include "hall_event.h"
#include "platform.h"
#include "svc_logger.h"
#include "svc_model_defaults.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGENT_RUN_SOCK_BUF AIRY_PATH_MAX

/* 一次 run 的执行上下文：阶段处理器共享，禁止跨 run 复用。 */
typedef struct {
    const char *prompt;
    const char *model;
    const cJSON *history;
    const char *gccp_answers;
    const cJSON *agent_spec;
    const char *agent_file;
    const char *session_id;
    const agent_run_event_sink_t *sink;

    char sess[AGENT_RUN_SESSION_ID_LEN];
    agent_run_session_t *active;
    cJSON *spec_owned;
    char mname[128];
    uint64_t run_start_ms;
    uint64_t seq;

    int gccp_round;
    cJSON *think;
    cJSON *trace;
    char *text;
    char *reason;
    uint64_t tokens;
    double cost;
    uint64_t think_ms;
    uint64_t llm_ms;
    uint64_t tool_ms;
    int rc;
    bool cancelled;
    char run_id[AGENT_RUN_SESSION_ID_LEN];
} run_ctx_t;

static void run_free(run_ctx_t *c)
{
    if (c->trace)
        cJSON_Delete(c->trace);
    if (c->think)
        cJSON_Delete(c->think);
    if (c->spec_owned)
        cJSON_Delete(c->spec_owned);
    AIRY_FREE(c->text);
    AIRY_FREE(c->reason);
    AIRY_MEMSET(c, 0, sizeof(*c));
}

/* ---- 依赖 daemon 的会话域辅助（本文件独占） ---- */

/* 依赖域统一出口：params 所有权移交；成功返回已解包的响应体（AIRY 分配）。 */
static char *run_rpc(const char *sock_name, const char *method, cJSON *params, int timeout_ms)
{
    if (!params)
        return NULL;
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return NULL;

    char sock[AGENT_RUN_SOCK_BUF];
    snprintf(sock, sizeof(sock), "%s", airy_runtime_dir_socket(sock_name));
    char *resp = NULL;
    int rc = daemon_rpc_call(sock, method, params_str, &resp, timeout_ms);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        AIRY_FREE(resp);
        return NULL;
    }
    return resp;
}

/* 记录一条 hall 事件（决策链写侧，best effort：写失败不阻塞主流程）。 */
static void agent_record_event(const char *session_id, const char *category, cJSON *content)
{
    if (!session_id || !session_id[0] || !content)
        return;
    char *content_str = cJSON_PrintUnformatted(content);
    if (!content_str)
        return;
    (void)hall_evt_write(session_id, category, NULL, content_str);
    AIRY_FREE(content_str);
}

/* GCCP 双思考：think_d.process（降级容忍：不可达时返回非零，调用方回退直接
 * LLM 工具循环，对话可用性不受影响）。 */
static int agent_think_process(const char *session_id, const char *prompt,
                               const char *gccp_answers, cJSON **out_think)
{
    *out_think = NULL;
    if (!prompt || !prompt[0])
        return -1;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON_AddStringToObject(params, "prompt", prompt);
    if (session_id && session_id[0])
        cJSON_AddStringToObject(params, "session_id", session_id);
    if (gccp_answers && gccp_answers[0])
        cJSON_AddStringToObject(params, "gccp_answers", gccp_answers);

    char *resp = run_rpc("think.sock", "process", params, AGENT_RUN_THINK_TIMEOUT_MS);
    if (!resp) {
        SVC_LOG_WARN("agent.run: think.process unreachable, degrading to direct LLM");
        return -1;
    }
    /* daemon_rpc_call 已提取 result 字段；think.process 的 result 是
     * 双思考结果的 JSON 字符串（与旧 gateway 解析一致）。 */
    cJSON *think = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!think || !cJSON_IsObject(think)) {
        if (think)
            cJSON_Delete(think);
        return -1;
    }
    *out_think = think;
    return 0;
}

/* 会话持久化：成功后把本轮问答写入 mem_d（best effort）。 */
static void agent_persist(const char *session_id, const char *user_prompt,
                          const char *assistant_text)
{
    if (!user_prompt)
        return;
    size_t data_len = strlen(user_prompt) + (assistant_text ? strlen(assistant_text) : 0) + 24;
    char *data = (char *)AIRY_MALLOC(data_len);
    cJSON *params = data ? cJSON_CreateObject() : NULL;
    cJSON *meta = params ? cJSON_CreateObject() : NULL;
    if (!meta) {
        AIRY_FREE(data);
        cJSON_Delete(params);
        return;
    }
    snprintf(data, data_len, "user: %s\nassistant: %s", user_prompt,
             assistant_text ? assistant_text : "");
    cJSON_AddStringToObject(params, "data", data);
    AIRY_FREE(data);
    cJSON_AddStringToObject(meta, "session_id", session_id ? session_id : "");
    cJSON_AddStringToObject(meta, "role", "agentrt");
    cJSON_AddItemToObject(params, "metadata", meta);

    char *resp = run_rpc("mem.sock", "write", params, AGENT_RUN_TOOL_TIMEOUT_MS);
    if (!resp)
        SVC_LOG_WARN("agent.run: mem.write failed (session=%s)", session_id ? session_id : "?");
    else
        AIRY_FREE(resp);
}

/* 顶层 "role:" 键解析（简单 YAML；复杂 YAML 应使用 JSON）。 */
static const char *yaml_role_value(const char *buf)
{
    const char *p = buf;
    while ((p = strstr(p, "role")) != NULL) {
        const char *q = p + 4;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q == ':')
            return q + 1;
        p = p + 4;
    }
    return NULL;
}

/* 从 agent_file 路径解析 agent spec（JSON / 简单 YAML / 纯文本首行）。 */
static cJSON *agent_spec_from_file(const char *path)
{
    if (!path || !path[0])
        return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) {
        SVC_LOG_WARN("agent.run: agent_file unreadable: %s", path);
        return NULL;
    }
    char *buf = (char *)AIRY_MALLOC(AGENT_RUN_AGENT_FILE_MAX + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, AGENT_RUN_AGENT_FILE_MAX, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *parsed = cJSON_Parse(buf);
    if (parsed) {
        if (cJSON_IsObject(parsed)) {
            AIRY_FREE(buf);
            return parsed;
        }
        cJSON_Delete(parsed);
    }

    const char *src = yaml_role_value(buf);
    if (!src)
        src = buf;
    size_t start = 0;
    while (src[start] == ' ' || src[start] == '\t')
        start++;
    size_t end = start;
    while (src[end] && src[end] != '\n' && src[end] != '\r' && src[end] != ',' &&
           end < AGENT_RUN_AGENT_FILE_MAX && end - start < 127)
        end++;
    while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t' || src[end - 1] == '"' ||
                           src[end - 1] == '\''))
        end--;

    cJSON *spec = NULL;
    if (end > start) {
        spec = cJSON_CreateObject();
        if (spec) {
            char role[128];
            size_t rlen = end - start;
            AIRY_MEMCPY(role, src + start, rlen);
            role[rlen] = '\0';
            cJSON_AddStringToObject(spec, "role", role);
            SVC_LOG_INFO("agent.run: agent spec built from agent_file (role=%s, file=%s)", role,
                         path);
        }
    } else {
        SVC_LOG_WARN("agent.run: agent_file contains no usable role: %s", path);
    }
    AIRY_FREE(buf);
    return spec;
}

/* ---- run 阶段编排 ---- */

/* 工具循环：产物所有权移交给上下文。 */
static void run_loop(run_ctx_t *c, const cJSON *history)
{
    agent_run_loop_args_t args = {c->prompt, history, c->mname, c->active, c->sink};
    agent_run_loop_result_t lr = {0};

    c->rc = agent_run_tool_loop(&args, &lr);
    c->llm_ms += lr.llm_ms;
    c->tool_ms += lr.tool_ms;
    c->trace = lr.trace;
    c->text = lr.text;
    c->reason = lr.reason;
    c->tokens = lr.tokens;
    c->cost = lr.cost;
}

/* 会话槽位 + 模型解析：调用方给定合法 sess_ ID 时沿用，否则生成新 ID。
 * 模型名属部署策略，机制核不注入厂商默认值——无来源即 fail-closed。 */
static int run_open(run_ctx_t *c)
{
    if (c->session_id && c->session_id[0] && strlen(c->session_id) < sizeof(c->sess) &&
        strncmp(c->session_id, "sess_", 5) == 0)
        AIRY_STRNCPY_TERM(c->sess, c->session_id, sizeof(c->sess));
    else
        agent_run_gen_session_id(c->sess, sizeof(c->sess));
    c->active = agent_run_register(c->sess);
    if (!c->active)
        return -1;
    agent_run_gen_run_id(c->active->run_id, sizeof(c->active->run_id));
    c->run_start_ms = airy_time_ms();

    if (svc_model_defaults_resolve(c->model, NULL, c->mname, sizeof(c->mname), NULL, 0) == 0)
        return 0;

    SVC_LOG_ERROR("agent.run: no model configured (model.yaml / AIRY_AGENT_MODEL)");
    cJSON *err = cJSON_CreateObject();
    if (err) {
        cJSON_AddNumberToObject(err, AIRY_RS_K_CODE, -32603);
        cJSON_AddStringToObject(err, AIRY_RS_K_MSG,
                                "no model configured: set $AIRY_CONFIG_DIR/model.yaml "
                                "or AIRY_AGENT_MODEL");
        cJSON_AddBoolToObject(err, AIRY_RS_K_RECOVER, 0);
        agent_run_emit_event(c->sink, &c->seq, c->active->run_id, c->sess, AIRY_RS_TYPE_ERROR, err);
    }
    agent_run_unregister(c->active);
    c->active = NULL;
    return -1;
}

/* run_start：决策链写侧 + run_stream 流式推送（同一锚点双写）。 */
static void run_announce(run_ctx_t *c)
{
    char pbuf[520];
    AIRY_STRNCPY_TERM(pbuf, c->prompt, sizeof(pbuf));

    cJSON *evt = cJSON_CreateObject();
    if (evt) {
        cJSON_AddStringToObject(evt, "event", "run_start");
        cJSON_AddStringToObject(evt, "prompt", pbuf);
        agent_record_event(c->sess, "chain", evt);
        cJSON_Delete(evt);
    }

    cJSON *rs = cJSON_CreateObject();
    if (!rs)
        return;
    cJSON_AddStringToObject(rs, AIRY_RS_K_PROMPT, pbuf);
    cJSON_AddStringToObject(rs, AIRY_RS_K_MODEL, c->mname);
    agent_run_emit_event(c->sink, &c->seq, c->active->run_id, c->sess, AIRY_RS_TYPE_RUN_START, rs);
}

/* 编排分支失败必须上抛为节点失败（rc=SUBAGENT_FAIL），原因走 response
 * 字段 + error 事件同一出口。 */
static void run_orch(run_ctx_t *c)
{
    char *err = NULL;

    c->rc = agent_run_orchestrate(c->agent_spec, c->prompt, &c->text, &err);
    c->trace = cJSON_CreateArray();
    if (c->rc == 0 && c->text) {
        AIRY_FREE(err);
        return;
    }
    AIRY_FREE(c->text);
    c->text = err ? err : AIRY_STRDUP("agent orchestration failed");
    c->rc = AGENT_RUN_RC_SUBAGENT_FAIL;
}

/* 双思考 DAG 计划：决策链 + run_stream 双写 plan 事件；计划注入 system 首部，
 * 并以轮次边界约束防止模型延续历史主题作答。 */
static void run_plan(run_ctx_t *c, const cJSON *plan)
{
    char *plan_str = cJSON_PrintUnformatted(plan);
    if (!plan_str)
        return;

    cJSON *evt = cJSON_CreateObject();
    if (evt) {
        char pbuf[1024];
        AIRY_STRNCPY_TERM(pbuf, plan_str, sizeof(pbuf));
        cJSON_AddStringToObject(evt, "event", "plan");
        cJSON_AddStringToObject(evt, "plan", pbuf);
        agent_record_event(c->sess, "chain", evt);
        cJSON_Delete(evt);
    }

    cJSON *messages = (c->history && cJSON_IsArray(c->history) &&
                       cJSON_GetArraySize((cJSON *)c->history) > 0)
                          ? cJSON_Duplicate(c->history, 1)
                          : cJSON_CreateArray();
    if (messages) {
        cJSON *sys = cJSON_CreateObject();
        if (sys) {
            char content[8192];
            int sn = snprintf(content, sizeof(content),
                              "You are executing a task under the AgentRT "
                              "dual-thinking system (GCCP goal confirmation + "
                              "GRAD plan critique). A verified action plan has "
                              "been produced. Follow this DAG plan strictly:\n%s\n"
                              "【轮次边界】历史消息（若有）仅供指代消解；回答必须"
                              "直接针对本任务的最后一条用户消息，禁止延续历史"
                              "主题作答。",
                              plan_str);
            cJSON_AddStringToObject(sys, "content",
                                    (sn > 0 && sn < (int)sizeof(content))
                                        ? content
                                        : "Execute the user request following "
                                          "the verified action plan.");
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddItemToArray(messages, sys);
        }
        cJSON *usr = cJSON_CreateObject();
        if (usr) {
            cJSON_AddStringToObject(usr, "role", "user");
            cJSON_AddStringToObject(usr, "content", c->prompt);
            cJSON_AddItemToArray(messages, usr);
        }
        run_loop(c, messages);
        cJSON_Delete(messages);
    }

    cJSON *ps = cJSON_CreateObject();
    if (ps) {
        char plbuf[2048];
        AIRY_STRNCPY_TERM(plbuf, plan_str, sizeof(plbuf));
        cJSON_AddStringToObject(ps, AIRY_RS_K_PLAN, plbuf);
        agent_run_emit_event(c->sink, &c->seq, c->active->run_id, c->sess, AIRY_RS_TYPE_PLAN, ps);
    }
    AIRY_FREE(plan_str);
}

/* 主对话：think_d 不可达或无 plan 一律降级为直接 LLM 工具循环（既有语义）。 */
static void run_think(run_ctx_t *c)
{
    uint64_t t0 = airy_time_ms();
    int think_rc = agent_think_process(c->sess, c->prompt, c->gccp_answers, &c->think);
    c->think_ms += airy_time_ms() - t0;
    if (think_rc != 0 || !c->think) {
        run_loop(c, c->history);
        return;
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(c->think, "gccp_need_interaction"))) {
        c->gccp_round = 1;
        c->trace = cJSON_CreateArray();
        c->rc = 0;
        SVC_LOG_INFO("agent.run: GCCP interaction round (session=%s)", c->sess);
        return;
    }
    const cJSON *plan = cJSON_GetObjectItem(c->think, "plan");
    if (plan)
        run_plan(c, plan);
    if (c->rc < 0)
        run_loop(c, c->history);
}

/* 决策链 tools 事件：工具轨迹按帧裁剪（单字段 160 字节，最多 32 条）。 */
static void run_tools_evt(run_ctx_t *c)
{
    cJSON *evt = cJSON_CreateObject();
    if (!evt)
        return;
    cJSON_AddStringToObject(evt, "event", "tools");

    cJSON *arr = cJSON_CreateArray();
    int n = cJSON_GetArraySize(c->trace);
    for (int i = 0; arr && i < n && i < 32; i++) {
        const cJSON *t = cJSON_GetArrayItem(c->trace, i);
        const cJSON *tool = cJSON_GetObjectItem(t, "tool");
        const cJSON *args = cJSON_GetObjectItem(t, "arguments");
        const cJSON *res = cJSON_GetObjectItem(t, "result");
        cJSON *item = cJSON_CreateObject();
        if (!item)
            continue;
        char buf[160];
        cJSON_AddStringToObject(item, "tool", cJSON_IsString(tool) ? tool->valuestring : "");
        AIRY_STRNCPY_TERM(buf, cJSON_IsString(args) ? args->valuestring : "", sizeof(buf));
        cJSON_AddStringToObject(item, "args", buf);
        AIRY_STRNCPY_TERM(buf, cJSON_IsString(res) ? res->valuestring : "", sizeof(buf));
        cJSON_AddStringToObject(item, "result", buf);
        cJSON_AddItemToArray(arr, item);
    }
    if (arr)
        cJSON_AddItemToObject(evt, "tools", arr);
    agent_record_event(c->sess, "chain", evt);
    cJSON_Delete(evt);
}

/* 决策链收尾：工具轨迹事件 + run_result（GCCP 交互轮不发 run_result）。 */
static void run_chain(run_ctx_t *c)
{
    if (c->trace && cJSON_IsArray(c->trace) && cJSON_GetArraySize(c->trace) > 0)
        run_tools_evt(c);
    if (c->gccp_round)
        return;

    cJSON *evt = cJSON_CreateObject();
    if (!evt)
        return;
    cJSON_AddStringToObject(evt, "event", "run_result");
    cJSON_AddNumberToObject(evt, "rc", c->rc);
    cJSON_AddNumberToObject(evt, "tokens", (double)c->tokens);
    cJSON_AddNumberToObject(evt, "cost", c->cost);
    char buf[520];
    AIRY_STRNCPY_TERM(buf, c->text ? c->text : "", sizeof(buf));
    cJSON_AddStringToObject(evt, "text", buf);
    agent_record_event(c->sess, "result", evt);
    cJSON_Delete(evt);
}

/* 组装 result：字段契约与旧 gateway 一致（CLI/TUI/SDK 兼容）。 */
static cJSON *run_fill(run_ctx_t *c)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        run_free(c);
        return NULL;
    }
    cJSON_AddStringToObject(result, "session_id", c->sess);
    cJSON_AddStringToObject(result, "response", c->text ? c->text : "");
    cJSON_AddNumberToObject(result, "tokens_used", (double)c->tokens);
    cJSON_AddNumberToObject(result, "cost_usd", c->cost);
    /* 思考链不再随 result 直出：仅经 message 帧 reasoning 字段按需（Alt+E）下发。 */
    cJSON_AddItemToObject(result, "tool_trace", c->trace ? c->trace : cJSON_CreateArray());
    c->trace = NULL;
    if (c->think) {
        /* 交互轮仅在成功时要求继续（失败/熔断轮不得引导用户进入第二段）。 */
        if (c->gccp_round && c->rc == 0) {
            const cJSON *qstr = cJSON_GetObjectItem(c->think, "gccp_questions");
            cJSON_AddBoolToObject(result, "interaction_required", 1);
            if (cJSON_IsString(qstr) && qstr->valuestring) {
                cJSON *q = cJSON_Parse(qstr->valuestring);
                if (q)
                    cJSON_AddItemToObject(result, "gccp_questions", q);
                else
                    cJSON_AddStringToObject(result, "gccp_questions", qstr->valuestring);
            }
        }
        cJSON_AddItemToObject(result, "thinking", c->think);
        c->think = NULL;
    }
    if (c->rc == 0 && !c->gccp_round)
        agent_persist(c->sess, c->prompt, c->text ? c->text : "");
    return result;
}

/* 流式收尾：message → run_end → error（顺序即客户端可见时序）。 */
static void run_tail(run_ctx_t *c)
{
    if (!c->sink || !c->sink->emit)
        return;

    if (c->rc == 0 && !c->gccp_round && c->text) {
        cJSON *msg = cJSON_CreateObject();
        if (msg) {
            cJSON_AddStringToObject(msg, AIRY_RS_K_ROLE, "assistant");
            cJSON_AddStringToObject(msg, AIRY_RS_K_CONTENT, c->text);
            /* 思考链随 message 下发但不作为对话内容：客户端仅按需（Alt+E）取用。 */
            if (c->reason && c->reason[0])
                cJSON_AddStringToObject(msg, AIRY_RS_K_REASONING, c->reason);
            agent_run_emit_event(c->sink, &c->seq, c->run_id, c->sess, AIRY_RS_TYPE_MESSAGE, msg);
        }
    }

    cJSON *end = cJSON_CreateObject();
    if (end) {
        cJSON_AddStringToObject(end, AIRY_RS_K_STATUS, c->cancelled ? "cancelled" :
                                                         (c->rc != 0 ? "failed" : "completed"));
        cJSON_AddNumberToObject(end, AIRY_RS_K_DURATION,
                                (double)(airy_time_ms() - c->run_start_ms));
        cJSON_AddNumberToObject(end, AIRY_RS_K_THINK_MS, (double)c->think_ms);
        cJSON_AddNumberToObject(end, AIRY_RS_K_LLM_MS, (double)c->llm_ms);
        cJSON_AddNumberToObject(end, AIRY_RS_K_TOOL_MS, (double)c->tool_ms);
        cJSON_AddNumberToObject(end, AIRY_RS_K_USE_TICKS, (double)c->tokens);
        agent_run_emit_event(c->sink, &c->seq, c->run_id, c->sess, AIRY_RS_TYPE_RUN_END, end);
    }

    if (c->rc != 0 && !c->cancelled) {
        cJSON *err = cJSON_CreateObject();
        if (err) {
            cJSON_AddNumberToObject(err, AIRY_RS_K_CODE, -32603);
            cJSON_AddStringToObject(err, AIRY_RS_K_MSG,
                                    (c->text && c->text[0]) ?
                                        c->text :
                                        "agent.run failed: tool loop exhausted or LLM service error");
            cJSON_AddBoolToObject(err, AIRY_RS_K_RECOVER, 1);
            agent_run_emit_event(c->sink, &c->seq, c->run_id, c->sess, AIRY_RS_TYPE_ERROR, err);
        }
    }
}

int agent_run_execute(const char *prompt, const char *model, const cJSON *history,
                      const char *gccp_answers, const cJSON *agent_spec,
                      const char *agent_file, const char *session_id,
                      const agent_run_event_sink_t *sink, cJSON **out_result)
{
    *out_result = NULL;
    if (!prompt || !prompt[0])
        return -1;

    run_ctx_t c = {0};
    c.prompt = prompt;
    c.model = model;
    c.history = history;
    c.gccp_answers = gccp_answers;
    c.agent_spec = agent_spec;
    c.agent_file = agent_file;
    c.session_id = session_id;
    c.sink = sink;
    c.rc = -1;

    if (run_open(&c) != 0)
        return -1;

    run_announce(&c);
    if (!c.agent_spec && c.agent_file) {
        c.spec_owned = agent_spec_from_file(c.agent_file);
        c.agent_spec = c.spec_owned;
    }
    if (c.agent_spec)
        run_orch(&c);
    else
        run_think(&c);

    if (!agent_run_is_cancelled(c.active) && c.rc == 0)
        run_chain(&c);

    c.cancelled = agent_run_is_cancelled(c.active);
    /* unregister 会释放 active：run_id 先落地本地副本，下方发帧禁止再触达
     * active。UAF 实锤（2026-09-09 0.1.13 实机回归）：释放后的 run_id 缓冲
     * 被响应文本分配复用，message/run_end 帧 run_id 被污染为内容片段。 */
    AIRY_STRNCPY_TERM(c.run_id, c.active->run_id, sizeof(c.run_id));
    agent_run_unregister(c.active);
    c.active = NULL;
    SVC_LOG_INFO("agent.run done (session=%s, rc=%d, tokens=%llu, cost=%.4f)", c.sess, c.rc,
                 (unsigned long long)c.tokens, c.cost);

    cJSON *result = run_fill(&c);
    if (!result)
        return -1;

    run_tail(&c);
    *out_result = result;
    int ret = c.cancelled ? 1 : c.rc;
    run_free(&c);
    return ret;
}
