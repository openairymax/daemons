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

#include "agent_review.h"
#include "agent_run_engine.h"
#include "agent_run_internal.h"
#include "agent_task_ledger.h"
#include "agent_turn_rec.h"
#include "agent_verify.h"

#include "airy_memory.h"
#include "airy_run_stream.h"
#include "daemon_rpc_client.h"
#include "err_class.h"
#include "error.h"
#include "hall_event.h"
#include "platform.h"
#include "safe_utf8.h"
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
    int rounds;                 /* 请求轮次预算（<=0 由策略缺省） */
    const char *resume_token;   /* 入参续跑令牌（借用，可 NULL） */
    const agent_run_task_meta_t *task; /* task.submit 元数据（借用，可 NULL） */
    char *resume_out;           /* 本轮生成的续跑令牌（自持有，可 NULL） */
    int rounds_used;
    int rounds_limit;

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
    uint64_t tokens_in;   /* D-11：prompt 分量（provider 实报） */
    uint64_t tokens_out;  /* D-11：completion 分量（provider 实报） */
    char fail_node[128];  /* D-10：熔断工具 id（affected_node） */
    int fail_denied;      /* D-10：失败前出现过审批/策略拒绝 */
    /* 上游失败明细（LLM 传输错误 / 'E' 帧码与文案）：终态错误帧优先采用，
     * 客户端据此区分「不可达 / 鉴权 / 超时 / 模型名错误」。 */
    char fail_msg[256];
    int fail_transport;   /* 上游失败是否为传输层（决定能否说「不可达」） */
    int fail_code;        /* 'E' 帧携带的真实上游码（0 = 未携带） */
    uint64_t think_ms;
    uint64_t llm_ms;
    uint64_t tool_ms;
    int rc;
    bool cancelled;
    /* D-13（E-20）：取消追溯在会话注销前捕获，终态落账与发帧共用。 */
    char cancel_by[64];
    uint64_t cancel_at_ms;
    char run_id[AGENT_RUN_SESSION_ID_LEN];
    cJSON *verify;        /* C-17：tool_d verifier 逐项证据（可 NULL） */
    char *criteria;       /* C-17：生效判据串（任务自带或计划派生，自持有） */
} run_ctx_t;

static void run_free(run_ctx_t *c)
{
    if (c->trace)
        cJSON_Delete(c->trace);
    if (c->think)
        cJSON_Delete(c->think);
    if (c->verify)
        cJSON_Delete(c->verify);
    if (c->spec_owned)
        cJSON_Delete(c->spec_owned);
    AIRY_FREE(c->criteria);
    AIRY_FREE(c->text);
    AIRY_FREE(c->reason);
    AIRY_FREE(c->resume_out);
    AIRY_MEMSET(c, 0, sizeof(*c));
}

static void run_criteria_prepare(run_ctx_t *c, const cJSON *plan);

/* C-18 账本侧：终态落 result，过程态落 progress；paused 可续跑，不算终态。
 * 先落账再发帧：流式帧可能因断连丢失，账本不能。
 * D-13（E-20）：取消终态额外记录发起者与时间，replay 才能区分
 * 用户取消与关机取消。 */
static void run_task_ledger(run_ctx_t *c, const char *state, const char *reason)
{
    const char *key = ledger_key(c->task, c->sess);
    bool cancelled_state = strcmp(state, AGENT_TASK_STATE_CANCELLED) == 0;
    bool terminal = cancelled_state || strcmp(state, AGENT_TASK_STATE_COMPLETED) == 0 ||
                    strcmp(state, AGENT_TASK_STATE_FAILED) == 0 ||
                    strcmp(state, AGENT_TASK_STATE_UNVERIFIED) == 0;
    if (cancelled_state)
        (void)ledger_cancel(key, c->task->plan_id, c->cancel_by, c->cancel_at_ms);
    else if (terminal)
        (void)ledger_result(key, c->task->plan_id, state);
    else
        (void)ledger_progress(key, c->task->plan_id, state, reason);
}

/* task_state 帧载荷（事件信封 data 段）。 */
static cJSON *run_task_state_data(run_ctx_t *c, const char *state, const char *reason,
                                  bool cancelled_state)
{
    cJSON *d = cJSON_CreateObject();
    if (!d)
        return NULL;
    cJSON_AddStringToObject(d, AIRY_RS_K_TASK_ID, c->task->task_id);
    if (c->task->plan_id[0])
        cJSON_AddStringToObject(d, AIRY_RS_K_PLAN_ID, c->task->plan_id);
    cJSON_AddStringToObject(d, AIRY_RS_K_STATE, state);
    if (reason && reason[0])
        cJSON_AddStringToObject(d, AIRY_RS_K_REASON, reason);
    if (cancelled_state) {
        if (c->cancel_by[0])
            cJSON_AddStringToObject(d, AIRY_RS_K_CANCELLED_BY, c->cancel_by);
        if (c->cancel_at_ms)
            cJSON_AddNumberToObject(d, AIRY_RS_K_CANCELLED_AT, (double)c->cancel_at_ms);
    }
    return d;
}

/* task_state 帧：C-15 任务时间线的唯一状态出口（仅 task.submit 携带 task
 * 元数据时发帧）。run_id 取 task_id，unregister 之后仍用本地副本。 */
static void run_task_state(run_ctx_t *c, const char *state, const char *reason)
{
    if (!c->task || !state || !state[0])
        return;
    bool cancelled_state = strcmp(state, AGENT_TASK_STATE_CANCELLED) == 0;
    run_task_ledger(c, state, reason);
    if (!c->sink || !c->sink->emit)
        return;
    cJSON *d = run_task_state_data(c, state, reason, cancelled_state);
    if (!d)
        return;
    const char *rid = c->run_id[0] ? c->run_id
                                   : (c->active ? c->active->run_id : c->task->task_id);
    agent_run_emit_event(c->sink, &c->seq, rid, c->sess, AIRY_RS_TYPE_TASK_STATE, d);
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

int agent_evt_ok(const char *line)
{
    if (!line)
        return 0;
    if (!utf8_validate(line, strlen(line)))
        return 0;
    cJSON *probe = cJSON_Parse(line);
    if (!probe)
        return 0;
    cJSON_Delete(probe);
    return 1;
}

/* 记录一条 hall 事件（决策链写侧，best effort：写失败不阻塞主流程）。
 * 落盘前校验 JSON 合法性与 UTF-8：链事件是后续复盘与回放的输入，写入
 * 一条坏行会让整段链路（NDJSON）解析失败，故宁可丢弃并告警。 */
static void agent_record_event(const char *session_id, const char *category, cJSON *content)
{
    if (!session_id || !session_id[0] || !content)
        return;
    char *content_str = cJSON_PrintUnformatted(content);
    if (!content_str)
        return;
    if (agent_evt_ok(content_str)) {
        (void)hall_evt_write(session_id, category, NULL, content_str);
    } else {
        SVC_LOG_WARN("agent.run: dropping malformed %s event (session=%s, %zu bytes)", category,
                     session_id, strlen(content_str));
    }
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

/* 会话持久化：成功后把本轮问答写入 mem_d（best effort）。
 * 一轮问答落两条记录（role=user/assistant，conv 与会话同值），契约由
 * agent_turn_rec 唯一给出；写失败只降级为日志，不回滚已完成的一轮。 */
static void agent_persist(const char *session_id, const char *user_prompt,
                          const char *assistant_text)
{
    cJSON *turn = agent_turn_rec(session_id, user_prompt, assistant_text);

    if (!turn)
        return;

    int n = cJSON_GetArraySize(turn);

    for (int i = 0; i < n; i++) {
        char *resp = run_rpc("mem.sock", "write", cJSON_GetArrayItem(turn, i),
                             AGENT_RUN_TOOL_TIMEOUT_MS);

        if (!resp)
            SVC_LOG_WARN("agent.run: mem.write failed (session=%s, record=%d)",
                         session_id ? session_id : "?", i);
        else
            AIRY_FREE(resp);
    }
    cJSON_Delete(turn);
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

/* agent_file 定位（D-11）：绝对路径原样使用；含 '/' 的相对路径以 AIRY_HOME
 * 为基准（守护进程 cwd 与客户端不同，按 cwd 解析会跨进程静默失效）；裸
 * agent id 解析为 $AIRY_HOME/agents/<id>.json。返回 0 表示路径可用。 */
static int agent_file_resolve(const char *in, char *out, size_t cap)
{
    const char *home = getenv("AIRY_HOME");
    int n;

    if (!in || !in[0] || !out || cap == 0)
        return -1;
    if (in[0] == '/')
        n = snprintf(out, cap, "%s", in);
    else if (!home || !home[0])
        return -1;
    else if (strchr(in, '/'))
        n = snprintf(out, cap, "%s/%s", home, in);
    else
        n = snprintf(out, cap, "%s/agents/%s.json", home, in);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* 读取 agent_file 全文；失败返回 NULL（real 回填解析后的绝对路径供诊断）。 */
static char *spec_read_file(const char *path, char *real, size_t cap)
{
    if (agent_file_resolve(path, real, cap) != 0) {
        SVC_LOG_ERROR("agent.run: agent_file unresolvable (AIRY_HOME unset or path too long): %s",
                      path ? path : "(null)");
        return NULL;
    }
    FILE *f = fopen(real, "rb");
    if (!f) {
        SVC_LOG_ERROR("agent.run: agent_file unreadable: %s (input=%s)", real, path);
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
    return buf;
}

/* 角色区间：从 src 起点裁掉首尾空白与引号，得到 [start,end) 区间。 */
static void spec_role_span(const char *src, size_t *out_start, size_t *out_end)
{
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
    *out_start = start;
    *out_end = end;
}

/* 角色名提取：role 值或首行，空角色视为不可用。 */
static cJSON *spec_role_obj(const char *buf, const char *real)
{
    const char *src = yaml_role_value(buf);
    if (!src)
        src = buf;
    size_t start = 0;
    size_t end = 0;
    spec_role_span(src, &start, &end);
    if (end <= start) {
        SVC_LOG_ERROR("agent.run: agent_file contains no usable role: %s", real);
        return NULL;
    }

    cJSON *spec = cJSON_CreateObject();
    if (!spec)
        return NULL;
    char role[128];
    size_t rlen = end - start;
    AIRY_MEMCPY(role, src + start, rlen);
    role[rlen] = '\0';
    cJSON_AddStringToObject(spec, "role", role);
    SVC_LOG_INFO("agent.run: agent spec built from agent_file (role=%s, file=%s)", role, real);
    return spec;
}

/* 从 agent_file 路径解析 agent spec（JSON / 简单 YAML / 纯文本首行）。 */
static cJSON *agent_spec_from_file(const char *path)
{
    char real[AGENT_RUN_SOCK_BUF];
    char *buf = spec_read_file(path, real, sizeof(real));
    if (!buf)
        return NULL;

    cJSON *parsed = cJSON_Parse(buf);
    if (parsed && cJSON_IsObject(parsed)) {
        AIRY_FREE(buf);
        return parsed;
    }
    if (parsed)
        cJSON_Delete(parsed);

    cJSON *spec = spec_role_obj(buf, real);
    AIRY_FREE(buf);
    return spec;
}

/* ---- run 阶段编排 ---- */

/* 工具循环：产物所有权移交给上下文。 */
static void run_loop(run_ctx_t *c, const cJSON *history)
{
    agent_run_loop_args_t args = {.prompt = c->prompt,
                                  .history = history,
                                  .model = c->mname,
                                  .session = c->active,
                                  .sink = c->sink,
                                  .rounds = c->rounds,
                                  .resume_token = c->resume_token,
                                  .allow_dangerous = c->task && c->task->allow_dangerous};
    agent_run_loop_result_t lr = {0};

    c->rc = agent_run_tool_loop(&args, &lr);
    c->llm_ms += lr.llm_ms;
    c->tool_ms += lr.tool_ms;
    c->trace = lr.trace;
    c->text = lr.text;
    c->reason = lr.reason;
    c->resume_out = lr.resume_token;
    c->rounds_used = lr.rounds_used;
    c->rounds_limit = lr.rounds_limit;
    c->tokens = lr.tokens;
    c->cost = lr.cost;
    c->tokens_in = lr.tokens_in;
    c->tokens_out = lr.tokens_out;
    AIRY_STRNCPY_TERM(c->fail_node, lr.fail_node, sizeof(c->fail_node));
    c->fail_denied = lr.fail_denied;
    AIRY_STRNCPY_TERM(c->fail_msg, lr.fail_msg, sizeof(c->fail_msg));
    c->fail_transport = lr.fail_transport;
    c->fail_code = lr.fail_code;
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
    /* task 提交以 task_id 作为信封 run_id：同一任务在 CLI/TUI/SDK 与重连
     * 后看到同一条时间线（C-15 N3），不再每次 run 换一个匿名 run_id。 */
    if (c->task && c->task->task_id[0])
        AIRY_STRNCPY_TERM(c->active->run_id, c->task->task_id, sizeof(c->active->run_id));
    else
        agent_run_gen_run_id(c->active->run_id, sizeof(c->active->run_id));
    c->run_start_ms = airy_time_ms();

    if (svc_model_defaults_resolve(c->model, NULL, c->mname, sizeof(c->mname), NULL, 0) == 0)
        return 0;

    SVC_LOG_ERROR("agent.run: no model configured (model.yaml / AIRY_AGENT_MODEL)");
    cJSON *err = cJSON_CreateObject();
    if (err) {
        cJSON_AddNumberToObject(err, AIRY_RS_K_CODE, -32603);
        cJSON_AddNumberToObject(err, AIRY_RS_K_ERROR_CODE, AIRY_ENOENT);
        cJSON_AddStringToObject(err, AIRY_RS_K_MSG,
                                "no model configured: set $AIRY_CONFIG_DIR/model.yaml "
                                "or AIRY_AGENT_MODEL");
        cJSON_AddBoolToObject(err, AIRY_RS_K_RECOVER, 0);
        /* 结构化失败载荷：retryable=false 且 origin=config，客户端据此显示
         * 「去配置模型」而不是「重试」——重试在无模型时永远不会成功。 */
        cJSON *fail = cJSON_CreateObject();
        if (fail) {
            cJSON_AddStringToObject(fail, AIRY_RS_K_ORIGIN, AIRY_RS_ORIGIN_CONFIG);
            cJSON_AddStringToObject(fail, AIRY_RS_K_REASON, AIRY_RS_REASON_CONFIG);
            cJSON_AddNumberToObject(fail, AIRY_RS_K_CODE, AIRY_ENOENT);
            cJSON_AddBoolToObject(fail, AIRY_RS_K_RETRYABLE, 0);
            cJSON_AddStringToObject(fail, AIRY_RS_K_NEXT_ACTION, AIRY_RS_NEXT_INSPECT);
            cJSON_AddItemToObject(err, AIRY_RS_K_FAILURE, fail);
        }
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
        agent_record_event(ledger_key(c->task, c->sess), "chain", evt);
        cJSON_Delete(evt);
    }

    cJSON *rs = cJSON_CreateObject();
    if (!rs)
        return;
    cJSON_AddStringToObject(rs, AIRY_RS_K_PROMPT, pbuf);
    cJSON_AddStringToObject(rs, AIRY_RS_K_MODEL, c->mname);
    /* task 元数据随 run_start 一次下发：客户端据此渲染任务头（模式与原因
     * 是服务端裁决结果，客户端不自行推断）。 */
    if (c->task) {
        cJSON_AddStringToObject(rs, AIRY_RS_K_TASK_ID, c->task->task_id);
        if (c->task->plan_id[0])
            cJSON_AddStringToObject(rs, AIRY_RS_K_PLAN_ID, c->task->plan_id);
        cJSON_AddStringToObject(rs, AIRY_RS_K_MODE, c->task->mode);
        cJSON_AddStringToObject(rs, AIRY_RS_K_MODE_REASON, c->task->mode_reason);
        cJSON_AddBoolToObject(rs, AIRY_RS_K_GCCP_CONFIRMED, c->task->gccp_confirmed);
        if (c->task->success_criteria && c->task->success_criteria[0])
            cJSON_AddStringToObject(rs, AIRY_RS_K_SUCCESS_CRITERIA,
                                    c->task->success_criteria);
    }
    agent_run_emit_event(c->sink, &c->seq, c->active->run_id, c->sess, AIRY_RS_TYPE_RUN_START, rs);
    run_task_state(c, AGENT_TASK_STATE_SUBMITTED, NULL);
    /* 蓝图是回放的起点：goal 与成功判据在提交时冻结，后续只看事件不改写。 */
    if (c->task)
        (void)ledger_plan(ledger_key(c->task, c->sess),
                                          c->task->plan_id, c->prompt,
                                          c->task->success_criteria);
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

/* 计划单步的可读文本：元素可能是字符串，或含 title/step/goal/action 的对象。 */
static const char *plan_step_text(const cJSON *step)
{
    if (cJSON_IsString(step))
        return step->valuestring;
    if (cJSON_IsObject(step)) {
        static const char *const keys[] = {"title", "step", "goal", "action", NULL};
        for (size_t i = 0; keys[i]; i++) {
            const cJSON *v = cJSON_GetObjectItem(step, keys[i]);
            if (cJSON_IsString(v) && v->valuestring && v->valuestring[0])
                return v->valuestring;
        }
    }
    return NULL;
}

/* step 帧：计划一经确认即逐条下发（step_status=planned）。客户端据此在
 * 第一个工具调用之前就能渲染步骤清单，而不是等执行完才拿到事后计划。 */
static void run_plan_steps(run_ctx_t *c, const cJSON *plan)
{
    const cJSON *steps = cJSON_IsObject(plan) ? cJSON_GetObjectItem(plan, "steps") : plan;
    if (!cJSON_IsArray(steps))
        return;
    int n = cJSON_GetArraySize(steps);
    for (int i = 0; i < n; i++) {
        cJSON *d = cJSON_CreateObject();
        if (!d)
            return;
        if (c->task) {
            cJSON_AddStringToObject(d, AIRY_RS_K_TASK_ID, c->task->task_id);
            if (c->task->plan_id[0])
                cJSON_AddStringToObject(d, AIRY_RS_K_PLAN_ID, c->task->plan_id);
        }
        cJSON_AddNumberToObject(d, AIRY_RS_K_STEP_INDEX, i);
        cJSON_AddNumberToObject(d, AIRY_RS_K_STEP_COUNT, n);
        cJSON_AddStringToObject(d, AIRY_RS_K_STEP_STATUS, "planned");
        const char *txt = plan_step_text(cJSON_GetArrayItem(steps, i));
        if (txt)
            cJSON_AddStringToObject(d, AIRY_RS_K_STEP, txt);
        agent_run_emit_event(c->sink, &c->seq, c->active->run_id, c->sess, AIRY_RS_TYPE_STEP, d);
    }
}

/* 计划宣告：决策链事件 + plan 帧。计划帧先于任何工具执行：旧顺序在
 * run_loop 之后才发 plan，客户端只能做事后回显，无法支撑「执行中」的步骤进度。 */
static void run_plan_announce(run_ctx_t *c, const cJSON *plan, const char *plan_str)
{
    cJSON *evt = cJSON_CreateObject();
    if (evt) {
        char pbuf[1024];
        AIRY_STRNCPY_TERM(pbuf, plan_str, sizeof(pbuf));
        cJSON_AddStringToObject(evt, "event", "plan");
        cJSON_AddStringToObject(evt, "plan", pbuf);
        agent_record_event(ledger_key(c->task, c->sess), "chain", evt);
        cJSON_Delete(evt);
    }

    cJSON *ps = cJSON_CreateObject();
    if (!ps)
        return;
    char plbuf[2048];
    AIRY_STRNCPY_TERM(plbuf, plan_str, sizeof(plbuf));
    cJSON_AddStringToObject(ps, AIRY_RS_K_PLAN, plbuf);
    if (c->task) {
        cJSON_AddStringToObject(ps, AIRY_RS_K_TASK_ID, c->task->task_id);
        if (c->task->plan_id[0])
            cJSON_AddStringToObject(ps, AIRY_RS_K_PLAN_ID, c->task->plan_id);
    }
    const cJSON *steps = cJSON_IsObject(plan) ? cJSON_GetObjectItem(plan, "steps") : plan;
    if (cJSON_IsArray(steps))
        cJSON_AddNumberToObject(ps, AIRY_RS_K_STEP_COUNT, cJSON_GetArraySize(steps));
    agent_run_emit_event(c->sink, &c->seq, c->active->run_id, c->sess, AIRY_RS_TYPE_PLAN, ps);
}

/* 计划注入：system 首部承接 DAG，user 收原文 prompt，历史仅供指代消解。 */
static void run_plan_messages(run_ctx_t *c, const char *plan_str)
{
    cJSON *messages = (c->history && cJSON_IsArray(c->history) &&
                       cJSON_GetArraySize((cJSON *)c->history) > 0)
                          ? cJSON_Duplicate(c->history, 1)
                          : cJSON_CreateArray();
    if (!messages)
        return;
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
    run_task_state(c, AGENT_TASK_STATE_EXECUTING, NULL);
    run_loop(c, messages);
    cJSON_Delete(messages);
}

/* 双思考 DAG 计划：先宣告计划与步骤再执行（C-15 顺序修复）；计划注入
 * system 首部，并以轮次边界约束防止模型延续历史主题作答。 */
static void run_plan(run_ctx_t *c, const cJSON *plan)
{
    char *plan_str = cJSON_PrintUnformatted(plan);
    if (!plan_str)
        return;

    /* C-17：计划到手即确定生效判据（请求自带或节点 postcondition 派生），
     * 使执行阶段与收尾阶段看到同一份判据。 */
    run_criteria_prepare(c, plan);
    run_plan_announce(c, plan, plan_str);
    run_plan_steps(c, plan);
    run_task_state(c, AGENT_TASK_STATE_PLANNED, NULL);
    run_plan_messages(c, plan_str);
    AIRY_FREE(plan_str);
}

/* 计划不可用：task 模式必须显式告知「规划失败」并降级直跑，而不是伪装成
 * 一次本来就不需要计划的任务。客户端据此区分两种情形并决定是否重试规划。 */
static void run_plan_unavailable(run_ctx_t *c, const char *reason)
{
    if (!c->task)
        return;
    cJSON *d = cJSON_CreateObject();
    if (d) {
        cJSON_AddStringToObject(d, AIRY_RS_K_TASK_ID, c->task->task_id);
        if (c->task->plan_id[0])
            cJSON_AddStringToObject(d, AIRY_RS_K_PLAN_ID, c->task->plan_id);
        cJSON_AddStringToObject(d, AIRY_RS_K_REASON, reason ? reason : "plan_unavailable");
        agent_run_emit_event(c->sink, &c->seq, c->active->run_id, c->sess,
                             AIRY_RS_TYPE_PLAN_UNAVAILABLE, d);
    }
    run_task_state(c, AGENT_TASK_STATE_EXECUTING, reason);
}

/* 主对话：think_d 不可达或无 plan 一律降级为直接 LLM 工具循环（既有语义）。 */
static void run_think(run_ctx_t *c)
{
    uint64_t t0 = airy_time_ms();
    int think_rc = agent_think_process(c->sess, c->prompt, c->gccp_answers, &c->think);
    c->think_ms += airy_time_ms() - t0;
    if (think_rc != 0 || !c->think) {
        run_plan_unavailable(c, "think_unavailable");
        run_loop(c, c->history);
        return;
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(c->think, "gccp_need_interaction"))) {
        c->gccp_round = 1;
        c->trace = cJSON_CreateArray();
        c->rc = 0;
        /* 交互轮没有执行：task 停在 paused，等客户端补齐 goal 后再提交。 */
        run_task_state(c, AGENT_TASK_STATE_PAUSED, "gccp_interaction_required");
        SVC_LOG_INFO("agent.run: GCCP interaction round (session=%s)", c->sess);
        return;
    }
    const cJSON *plan = cJSON_GetObjectItem(c->think, "plan");
    if (plan) {
        run_plan(c, plan);
    } else {
        run_plan_unavailable(c, "plan_missing");
        run_loop(c, c->history);
        return;
    }
    /* 预算耗尽已是终态（trace/正文完整移交），不得当作「计划段未执行」
     * 再降级重跑一遍——那会把已消耗的轮次与产物整体作废。task 模式下
     * 计划段失败也不重放：第二次 run_loop 会掩盖首次失败的真实原因。 */
    if (c->rc < 0 && c->rc != AIRY_EBUDGET && !c->task)
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
        buf[utf8_prefix_len(buf, strlen(buf))] = '\0';
        cJSON_AddStringToObject(item, "args", buf);
        AIRY_STRNCPY_TERM(buf, cJSON_IsString(res) ? res->valuestring : "", sizeof(buf));
        buf[utf8_prefix_len(buf, strlen(buf))] = '\0';
        cJSON_AddStringToObject(item, "result", buf);
        cJSON_AddItemToArray(arr, item);
    }
    if (arr)
        cJSON_AddItemToObject(evt, "tools", arr);
    agent_record_event(ledger_key(c->task, c->sess), "chain", evt);
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
    cJSON_AddStringToObject(evt, "state", run_state_name(c->rc, c->cancelled));
    cJSON_AddNumberToObject(evt, "tokens", (double)c->tokens);
    cJSON_AddNumberToObject(evt, "cost", c->cost);
    char buf[520];
    AIRY_STRNCPY_TERM(buf, c->text ? c->text : "", sizeof(buf));
    cJSON_AddStringToObject(evt, "text", buf);
    const char *key = ledger_key(c->task, c->sess);
    agent_record_event(key, "result", evt);
    cJSON_Delete(evt);

    /* 失败与取消必留 issue（C-18）：看板/回放不能只看到文本结果，
     * 还要能判读字段级原因（rc + 引擎给出的可读说明）。 */
    if (c->task) {
        if (c->cancelled)
            (void)ledger_issue(key, c->rc, "cancelled");
        else if (c->rc == AGENT_RUN_RC_UNVERIFIED)
            (void)ledger_issue(key, c->rc, "unverified");
        else if (c->rc != 0)
            (void)ledger_issue(key, c->rc, c->text);
        else
            (void)ledger_issue(key, 0, "ok"); /* 成功也留 issue 结论，回放三类齐全 */
    }
}

/* C-17：模型自报完成后，由 tool_d verifier 在独立上下文只读复核判据。
 * 关键纪律：[TASK:DONE] 之类的自报不构成完成依据；只有逐项取证通过才是
 * 完成，未过即确定性失败（rc=UNMET），无法取证则记 unverified。 */
static void run_verify(run_ctx_t *c)
{
    cJSON *verification = NULL;
    int vrc = agent_verify_apply(c->criteria, &verification);
    c->verify = verification;
    if (vrc != 0)
        c->rc = vrc;
}

/* R16 / C-4：判据复核通过后再做一次语义复核（t2 语义审查 + t1-f 终裁）。
 * 整链由 think_d 承载（think.review pipeline 档，机制在 coreloopthree）；
 * think_d 不可达或无模型时降级 SKIP，既有结果不变。终裁拒绝按未达判据
 * 收尾。 */
static void run_review(run_ctx_t *c)
{
    cJSON *art = cJSON_CreateObject();
    if (!art)
        return;
    cJSON_AddStringToObject(art, "text", c->text);
    if (c->verify)
        cJSON_AddItemToObject(art, "verification", cJSON_Duplicate(c->verify, 1));
    char *art_json = cJSON_PrintUnformatted(art);
    cJSON_Delete(art);
    char *facts = c->verify ? cJSON_PrintUnformatted(c->verify) : NULL;
    if (!art_json) {
        AIRY_FREE(facts);
        return;
    }

    agent_review_result_t rv;
    int arc = agent_rev_apply(c->prompt, c->text, art_json, facts, &rv);
    AIRY_FREE(facts);
    AIRY_FREE(art_json);
    if (arc != 0 || rv.verdict == AIRY_REVIEW_SKIP)
        return;

    const char *key = c->task ? ledger_key(c->task, c->sess) : c->sess;
    cJSON *evt = cJSON_CreateObject();
    if (evt) {
        cJSON_AddStringToObject(evt, "event", "review");
        cJSON_AddNumberToObject(evt, "verdict", rv.verdict);
        cJSON_AddStringToObject(evt, "reason", rv.reason);
        agent_record_event(key, "review", evt);
        cJSON_Delete(evt);
    }
    if (rv.verdict != AIRY_REVIEW_REJECT)
        return;
    AIRY_FREE(c->reason);
    c->reason = rv.reason[0] ? AIRY_STRDUP(rv.reason) : NULL;
    c->rc = AGENT_RUN_RC_UNMET;
}

/* 判据来源二选一：请求自带优先；task 模式缺失时由计划节点 postcondition
 * 汇总派生（用户未给判据的唯一补全路径）。chat 轮不复核。 */
static void run_criteria_prepare(run_ctx_t *c, const cJSON *plan)
{
    if (!c->task || strcmp(c->task->mode, AGENT_TASK_MODE_TASK) != 0)
        return;
    if (c->criteria && c->criteria[0])
        return;
    AIRY_FREE(c->criteria);
    c->criteria = criteria_from_plan(plan);
}

/* D-10：失败载荷唯一装配点。code 原样保留，分层只补语义字段与建议动作。 */
static cJSON *run_fail_obj(const run_ctx_t *c)
{
    agent_fail_desc_t d = agent_fail_desc(c->rc, c->fail_denied);
    cJSON *f = cJSON_CreateObject();
    const cJSON *id = c->spec_owned ? cJSON_GetObjectItem(c->spec_owned, "id") : NULL;
    const char *node = c->fail_node[0] ? c->fail_node : "agent_d";

    if (!f)
        return NULL;
    /* 上游失败按实际错误类别再细分：传输层才说「不可达」；可达但瞬时故障
     * （5xx/空响应/解析失败/限流）与配置类（鉴权/模型名/参数/策略拒绝）
     * 都归 provider_error，但只有配置类不可重试——原样重发对瞬时故障有效，
     * 对配置错误只会重复失败。 */
    if (c->rc == AGENT_RUN_RC_LLM_FAIL) {
        airy_err_class_t cls = airy_err_class_of(c->fail_code);
        d.origin = AIRY_RS_ORIGIN_PROVIDER;
        d.reason = c->fail_transport ? AIRY_RS_REASON_PROVIDER : AIRY_RS_REASON_PROVIDER_ERR;
        /* 未携带真实上游码时退回传输判据：无码的 LLM_FAIL 只可能来自
         * llm_d 不可达或协议拒绝，前者可重试、后者不可。 */
        d.retryable = c->fail_code != 0
                          ? (cls != AIRY_ERR_CLASS_CONFIG && cls != AIRY_ERR_CLASS_POLICY)
                          : c->fail_transport;
        d.next_action = d.retryable ? AIRY_RS_NEXT_RETRY : AIRY_RS_NEXT_INSPECT;
    }
    if (!c->fail_node[0] && cJSON_IsString(id) && id->valuestring[0])
        node = id->valuestring;
    cJSON_AddStringToObject(f, AIRY_RS_K_ORIGIN, d.origin);
    cJSON_AddNumberToObject(f, AIRY_RS_K_CODE, c->rc);
    cJSON_AddStringToObject(f, AIRY_RS_K_REASON, d.reason);
    cJSON_AddBoolToObject(f, AIRY_RS_K_RETRYABLE, d.retryable);
    cJSON_AddNumberToObject(f, AIRY_RS_K_BUDGET_USED, c->rounds_used);
    cJSON_AddNumberToObject(f, AIRY_RS_K_BUDGET_TOTAL, c->rounds_limit);
    cJSON_AddStringToObject(f, AIRY_RS_K_NODE, node);
    cJSON_AddStringToObject(f, AIRY_RS_K_NEXT_ACTION, d.next_action);
    /* C-17：逐项证据随失败载荷下发（type/expect/actual/pass），客户端无需
     * 猜测哪条判据未过，也不必回读 verifier。 */
    if (c->verify)
        cJSON_AddItemToObject(f, AIRY_RS_K_VERIFICATION, cJSON_Duplicate(c->verify, 1));
    return f;
}

/* D-11：usage 结构化。provider 实报优先；无实报按字符数估算并标注来源，
 * 避免客户端把缺失计量误读成零消耗。 */
static cJSON *run_usage_obj(const run_ctx_t *c)
{
    uint64_t total = c->tokens;
    uint64_t pin = c->tokens_in;
    uint64_t pout = c->tokens_out;
    const char *src = AIRY_RS_USAGE_PROVIDER;
    cJSON *u;

    if (total == 0) {
        pin = (uint64_t)((c->prompt ? strlen(c->prompt) : 0) / 4);
        pout = (uint64_t)((c->text ? strlen(c->text) : 0) / 4);
        total = pin + pout;
        src = AIRY_RS_USAGE_ESTIMATE;
    } else if (pin + pout == 0) {
        pin = total;
    }
    u = cJSON_CreateObject();
    if (!u)
        return NULL;
    cJSON_AddNumberToObject(u, AIRY_RS_K_USAGE_PROMPT, (double)pin);
    cJSON_AddNumberToObject(u, AIRY_RS_K_USAGE_COMP, (double)pout);
    cJSON_AddNumberToObject(u, AIRY_RS_K_USAGE_TOTAL, (double)total);
    cJSON_AddStringToObject(u, AIRY_RS_K_USAGE_SRC, src);
    return u;
}

/* 契约字段：会话/任务元数据、终态、失败载荷、用量与续跑令牌。 */
static void run_fill_meta(run_ctx_t *c, cJSON *result)
{
    cJSON_AddStringToObject(result, "session_id", c->sess);
    /* task 元数据与事件信封同源：客户端用同一 task_id 关联结果与时间线。 */
    if (c->task) {
        cJSON_AddStringToObject(result, AIRY_RS_K_TASK_ID, c->task->task_id);
        if (c->task->plan_id[0])
            cJSON_AddStringToObject(result, AIRY_RS_K_PLAN_ID, c->task->plan_id);
        cJSON_AddStringToObject(result, AIRY_RS_K_MODE, c->task->mode);
        cJSON_AddStringToObject(result, AIRY_RS_K_MODE_REASON, c->task->mode_reason);
        cJSON_AddBoolToObject(result, AIRY_RS_K_GCCP_CONFIRMED, c->task->gccp_confirmed);
        if (c->criteria && c->criteria[0])
            cJSON_AddStringToObject(result, AIRY_RS_K_SUCCESS_CRITERIA, c->criteria);
    }
    /* C-17：复核证据与 result 同源（成功含 ok=true，未过含逐项 actual）。 */
    if (c->verify)
        cJSON_AddItemToObject(result, AIRY_RS_K_VERIFICATION, cJSON_Duplicate(c->verify, 1));
    cJSON_AddStringToObject(result, "response", c->text ? c->text : "");
    /* 一等终态（C-14）：status 是客户端渲染「继续/放弃」的唯一判据，
     * error_code 保留原始码供排障；Paused 时 resume_token 随结果下发，
     * 客户端凭它续跑而不必重发全量历史。 */
    cJSON_AddStringToObject(result, "status", run_state_name(c->rc, c->cancelled));
    if (c->rc != 0) {
        cJSON_AddNumberToObject(result, "error_code", c->rc);
        /* D-10：非取消失败随结果带结构化载荷，CLI/离线调用方无需再解析文案。 */
        if (!c->cancelled) {
            cJSON *fail = run_fail_obj(c);
            if (fail)
                cJSON_AddItemToObject(result, AIRY_RS_K_FAILURE, fail);
        }
    }
    cJSON *usage = run_usage_obj(c);
    if (usage)
        cJSON_AddItemToObject(result, AIRY_RS_K_USAGE, usage);
    cJSON_AddNumberToObject(result, AIRY_RS_K_ROUNDS_USED, c->rounds_used);
    cJSON_AddNumberToObject(result, AIRY_RS_K_ROUNDS_LIMIT, c->rounds_limit);
    if (c->resume_out) {
        cJSON_AddStringToObject(result, AIRY_RS_K_RESUME_TOKEN, c->resume_out);
        AIRY_FREE(c->resume_out);
        c->resume_out = NULL;
    }
    cJSON_AddNumberToObject(result, "tokens_used", (double)c->tokens);
    cJSON_AddNumberToObject(result, "cost_usd", c->cost);
}

/* 轨迹与思考链：tool_trace 整棵移交；交互轮问题集按成功与否决定是否引导。 */
static void run_fill_trace(run_ctx_t *c, cJSON *result)
{
    /* 思考链不再随 result 直出：仅经 message 帧 reasoning 字段按需（Alt+E）下发。 */
    cJSON_AddItemToObject(result, "tool_trace", c->trace ? c->trace : cJSON_CreateArray());
    c->trace = NULL;
    if (!c->think)
        return;
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

/* 组装 result：字段契约与旧 gateway 一致（CLI/TUI/SDK 兼容）。 */
static cJSON *run_fill(run_ctx_t *c)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        run_free(c);
        return NULL;
    }
    run_fill_meta(c, result);
    run_fill_trace(c, result);
    if (c->rc == 0 && !c->gccp_round)
        agent_persist(c->sess, c->prompt, c->text ? c->text : "");
    return result;
}

/* task 终态帧与 run_end.status 同源：paused 表示可凭 resume_token 继续，
 * 客户端不得把 paused 当失败渲染；交互轮已单独发过 paused，不重复。 */
static void run_tail_state(run_ctx_t *c)
{
    if (!c->task || c->gccp_round)
        return;
    const char *ts =
        c->cancelled ? AGENT_TASK_STATE_CANCELLED : run_state_name(c->rc, c->cancelled);
    run_task_state(c, ts, NULL);
}

/* C-17：判据无法取证时终态是 unverified，但正文仍需交付——否则用户拿到
 * 结论却看不到内容，无从判断是否需要补判据。 */
static void run_tail_msg(run_ctx_t *c)
{
    if (c->rc != 0 && c->rc != AGENT_RUN_RC_UNVERIFIED)
        return;
    if (c->gccp_round || !c->text)
        return;
    cJSON *msg = cJSON_CreateObject();
    if (!msg)
        return;
    cJSON_AddStringToObject(msg, AIRY_RS_K_ROLE, "assistant");
    cJSON_AddStringToObject(msg, AIRY_RS_K_CONTENT, c->text);
    /* 思考链随 message 下发但不作为对话内容：客户端仅按需（Alt+E）取用。 */
    if (c->reason && c->reason[0])
        cJSON_AddStringToObject(msg, AIRY_RS_K_REASONING, c->reason);
    agent_run_emit_event(c->sink, &c->seq, c->run_id, c->sess, AIRY_RS_TYPE_MESSAGE, msg);
}

static void run_tail_end(run_ctx_t *c, const cJSON *res, const cJSON *rt, bool resumable)
{
    cJSON *end = cJSON_CreateObject();
    if (!end)
        return;
    cJSON_AddStringToObject(end, AIRY_RS_K_STATUS, run_state_name(c->rc, c->cancelled));
    cJSON_AddNumberToObject(end, AIRY_RS_K_DURATION, (double)(airy_time_ms() - c->run_start_ms));
    cJSON_AddNumberToObject(end, AIRY_RS_K_THINK_MS, (double)c->think_ms);
    cJSON_AddNumberToObject(end, AIRY_RS_K_LLM_MS, (double)c->llm_ms);
    cJSON_AddNumberToObject(end, AIRY_RS_K_TOOL_MS, (double)c->tool_ms);
    cJSON_AddNumberToObject(end, AIRY_RS_K_USE_TICKS, (double)c->tokens);
    cJSON_AddNumberToObject(end, AIRY_RS_K_ROUNDS_USED, c->rounds_used);
    cJSON_AddNumberToObject(end, AIRY_RS_K_ROUNDS_LIMIT, c->rounds_limit);
    if (resumable)
        cJSON_AddStringToObject(end, AIRY_RS_K_RESUME_TOKEN, rt->valuestring);
    /* C-17：复核证据随终态帧下发（成功亦包含 ok=true），客户端据此逐条
     * 呈现判据结果，不再依赖正文字样推断完成。 */
    if (c->verify)
        cJSON_AddItemToObject(end, AIRY_RS_K_VERIFICATION, cJSON_Duplicate(c->verify, 1));
    /* D-10/D-11：终态帧与 result 同源，失败载荷与 usage 一处装配两处下发。 */
    if (c->rc != 0 && !c->cancelled) {
        cJSON *fail = run_fail_obj(c);
        if (fail)
            cJSON_AddItemToObject(end, AIRY_RS_K_FAILURE, fail);
    }
    cJSON *usage = run_usage_obj(c);
    if (usage)
        cJSON_AddItemToObject(end, AIRY_RS_K_USAGE, usage);
    const cJSON *tr = res ? cJSON_GetObjectItem(res, AIRY_RS_K_TOOL_TRACE) : NULL;
    if (cJSON_IsArray(tr) && cJSON_GetArraySize(tr) > 0)
        cJSON_AddItemToObject(end, AIRY_RS_K_TOOL_TRACE, cJSON_Duplicate(tr, 1));
    agent_run_emit_event(c->sink, &c->seq, c->run_id, c->sess, AIRY_RS_TYPE_RUN_END, end);
}

static void run_tail_err(run_ctx_t *c, const cJSON *rt, bool resumable)
{
    cJSON *err = cJSON_CreateObject();
    if (!err)
        return;
    cJSON_AddNumberToObject(err, AIRY_RS_K_CODE, -32603);
    cJSON_AddNumberToObject(err, AIRY_RS_K_ERROR_CODE, c->rc);
    /* 明细优先：上游失败只在这里能还原成可操作信息（哪一层、什么码、
     * provider 原话），不回落到整类兜底文案。 */
    cJSON_AddStringToObject(err, AIRY_RS_K_MSG,
                            c->fail_msg[0] ? c->fail_msg
                                           : ((c->text && c->text[0]) ? c->text
                                                                      : run_rc_msg(c->rc)));
    cJSON_AddBoolToObject(err, AIRY_RS_K_RECOVER, 1);
    cJSON_AddNumberToObject(err, AIRY_RS_K_ROUNDS_USED, c->rounds_used);
    cJSON_AddNumberToObject(err, AIRY_RS_K_ROUNDS_LIMIT, c->rounds_limit);
    if (resumable)
        cJSON_AddStringToObject(err, AIRY_RS_K_RESUME_TOKEN, rt->valuestring);
    cJSON *fail = run_fail_obj(c);
    if (fail)
        cJSON_AddItemToObject(err, AIRY_RS_K_FAILURE, fail);
    agent_run_emit_event(c->sink, &c->seq, c->run_id, c->sess, AIRY_RS_TYPE_ERROR, err);
}

/* 流式收尾：message → run_end → error（顺序即客户端可见时序）。
 * res 为已装配的 RPC result：工具轨迹在那里（fill 已接管所有权），
 * 终态帧按需深拷贝一份，流式客户端才能看到参数与结果正文。 */
static void run_tail(run_ctx_t *c, const cJSON *res)
{
    /* D-13：终态落账与流式发帧解耦。同步 task.submit 没有 sink，旧实现
     * 直接返回，决策链只留 run_result，看板与回放永远停在 running，与
     * 流式路径形成两套口径。落账无条件，发帧由 run_task_state 自行判断。 */
    if (!c->sink || !c->sink->emit) {
        run_tail_state(c);
        return;
    }
    /* 续跑令牌已在 run_fill 移交 result，终态帧从那里取同一份文本：
     * run_end 与 error 都必须带，客户端才知道暂停后凭何继续。 */
    const cJSON *rt = res ? cJSON_GetObjectItem(res, AIRY_RS_K_RESUME_TOKEN) : NULL;
    const bool resumable = cJSON_IsString(rt) && rt->valuestring && rt->valuestring[0];

    run_tail_msg(c);
    run_tail_state(c);
    run_tail_end(c, res, rt, resumable);
    if (c->rc != 0 && !c->cancelled)
        run_tail_err(c, rt, resumable);
}

/* D-11：显式给出 agent_file 却解析不了，必须失败而非按默认 spec 继续——
 * 静默降级会让调用方以为指定了角色，实际跑的是别的角色。 */
static int exec_spec(run_ctx_t *c)
{
    if (c->agent_spec || !c->agent_file)
        return 0;
    c->spec_owned = agent_spec_from_file(c->agent_file);
    if (!c->spec_owned)
        return 1;
    c->agent_spec = c->spec_owned;
    return 0;
}

/* 主执行：显式角色走编排，否则走思考链。 */
static void exec_body(run_ctx_t *c)
{
    if (exec_spec(c))
        c->rc = AIRY_ENOENT;
    else if (c->agent_spec)
        run_orch(c);
    else
        run_think(c);
}

/* D-13（E-20）：注销前取走取消追溯，终态落账与 task_state 帧共用。 */
static void exec_cancel(run_ctx_t *c)
{
    c->cancelled = agent_run_is_cancelled(c->active);
    if (!c->cancelled)
        return;
    const char *by = NULL;
    run_cancel_meta(c->active, &by, &c->cancel_at_ms);
    if (by && by[0])
        AIRY_STRNCPY_TERM(c->cancel_by, by, sizeof(c->cancel_by));
}

/* 终态缺口：空输出 → EMPTY；task 轮无判据 → unverified。
 * 完成判据外部化（C-17）：空输出已判 EMPTY，无需再复核；task 模式一条
 * 判据都拿不到时记 unverified——可交付但不得冒充 completed。 */
static void verdict_gap(run_ctx_t *c)
{
    /* 无输出终态（0.1.21 L-9）：编排成功但终局正文为空，不是「成功但空白」。
     * GCCP 交互轮以问题集为产物、取消轮由取消语义覆盖，二者不判空。 */
    if (c->rc != 0 || c->cancelled || c->gccp_round)
        return;
    if (!c->text || !c->text[0]) {
        c->rc = AGENT_RUN_RC_EMPTY;
        return;
    }
    if (c->task && strcmp(c->task->mode, AGENT_TASK_MODE_TASK) == 0 &&
        (!c->criteria || !c->criteria[0]))
        c->rc = AGENT_RUN_RC_UNVERIFIED;
}

/* 终态判定：缺口先补齐，有判据的 task 轮必须经 verifier 复核，
 * 「模型说完成」不进入终态判定。
 * R16：判据取证通过后再做语义复核（产物 vs 目标），判定作为执行事件落链。 */
static void exec_verdict(run_ctx_t *c)
{
    verdict_gap(c);
    if (c->rc != 0 || c->cancelled || c->gccp_round || !c->criteria || !c->criteria[0])
        return;
    run_verify(c);
    if (c->rc == 0)
        run_review(c);
}

/* 收尾：先落 run_id 副本再注销，装配 result 后发终态帧。 */
static int exec_finish(run_ctx_t *c, cJSON **out_result)
{
    /* unregister 会释放 active：run_id 先落地本地副本，下方发帧禁止再触达
     * active。UAF 实锤（2026-09-09 0.1.13 实机回归）：释放后的 run_id 缓冲
     * 被响应文本分配复用，message/run_end 帧 run_id 被污染为内容片段。 */
    AIRY_STRNCPY_TERM(c->run_id, c->active->run_id, sizeof(c->run_id));
    agent_run_unregister(c->active);
    c->active = NULL;
    SVC_LOG_INFO("agent.run done (session=%s, rc=%d, tokens=%llu, cost=%.4f)", c->sess, c->rc,
                 (unsigned long long)c->tokens, c->cost);

    cJSON *result = run_fill(c);
    if (!result)
        return -1;

    run_tail(c, result);
    *out_result = result;
    int ret = c->cancelled ? 1 : c->rc;
    run_free(c);
    return ret;
}

int agent_run_execute(const char *prompt, const char *model, const cJSON *history,
                      const char *gccp_answers, const cJSON *agent_spec,
                      const char *agent_file, const char *session_id, int rounds,
                      const char *resume_token, const agent_run_task_meta_t *task,
                      const agent_run_event_sink_t *sink, cJSON **out_result)
{
    *out_result = NULL;
    /* 续跑请求只带令牌不带原 prompt 也合法：历史由令牌摘要重建。 */
    if ((!prompt || !prompt[0]) && (!resume_token || !resume_token[0]))
        return -1;

    run_ctx_t c = {0};
    c.prompt = prompt;
    c.model = model;
    c.history = history;
    c.gccp_answers = gccp_answers;
    c.agent_spec = agent_spec;
    c.agent_file = agent_file;
    c.session_id = session_id;
    c.rounds = rounds;
    c.resume_token = resume_token;
    c.task = task;
    c.sink = sink;
    c.rc = -1;
    /* C-17：请求自带判据先落到 ctx（自持有一份，生命周期与本轮一致）；计划
     * 派生只在缺失时补位。 */
    if (task && task->success_criteria && task->success_criteria[0])
        c.criteria = AIRY_STRDUP(task->success_criteria);

    if (run_open(&c) != 0) {
        AIRY_FREE(c.criteria);
        return -1;
    }

    run_announce(&c);
    exec_body(&c);
    exec_cancel(&c);
    exec_verdict(&c);

    /* 终态必写决策链（0.1.21 L-9）：旧行为只在成功时写 run_result，失败与
     * 取消在链上无终局，看板与回放永远停在 running，只能靠日志猜结果。
     * GCCP 交互轮不是终态，由 run_chain 内部跳过 run_result。 */
    run_chain(&c);
    return exec_finish(&c, out_result);
}
