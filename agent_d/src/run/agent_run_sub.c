/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_run_sub.c
 * @brief 子任务委派机制件（N-8）：委派工具识别、子 run 生命周期与事件树。
 *
 * 一次委派 = 一个 delegate 任务：create 建任务、assign 跑子 run、collect
 * 取结论、destroy 收账；取消经 airy_delegate_cancel 置任务态，不用外层
 * 标志位假装。子 run 复用同一个 ReAct 工具循环，但只带自己的上下文——父
 * 会话历史不重放，避免子任务被父任务的中间过程带偏。
 *
 * 事件侧：子 run 的帧与父 run 共用一条序号与一个通道，靠 data 上的
 * sub_id/depth 归组成子树；委派本身只多出 start/end 两帧。
 */

#include "agent_run_sub.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "agent_run_loop.h"
#include "agent_sub_policy.h"
#include "airy_delegate.h"
#include "airy_memory.h"
#include "airy_run_stream.h"
#include "error.h"
#include "platform.h"

/* 子任务标识序号：只在进程内区分同一次 run 的多次委派，不承担全局唯一。 */
static airy_atomic_int_t g_sub_seq;

typedef struct {
    const struct agent_run_session_s *session;
    const agent_run_event_sink_t *up;
    agent_run_event_sink_t own;
    uint64_t *seq;
    const char *model;
    const char *run_id;
    const char *sess_id;
    const char *agent_id; /* 审计主体：沿父 run 的命名代理 id，留痕五字段不缺位 */
    int allow_dangerous;
    int depth;
    uint64_t budget;
    airy_delegate_task_t *task;
    char sub_id[AGENT_SUB_ID_LEN];
    uint64_t local_seq;
    const char *stop;
    uint64_t tokens;
    uint64_t tokens_in;
    uint64_t tokens_out;
    uint64_t llm_ms;
    uint64_t tool_ms;
    double cost;
} sub_ctx_t;

static char *sub_err(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return AIRY_STRDUP(buf);
}

/* 子任务的帧在离开本层前打上归属：已带 sub_id 的帧说明来自更深的委派，
 * 内层归属优先，不被外层覆盖。 */
static void sub_emit(const char *type, cJSON *env, void *ud)
{
    sub_ctx_t *c = (sub_ctx_t *)ud;
    cJSON *data;
    if (!c || !c->up || !c->up->emit || !env)
        return;
    data = cJSON_GetObjectItem(env, AIRY_RS_K_DATA);
    if (!cJSON_IsObject(data)) {
        data = cJSON_CreateObject();
        if (!data)
            return;
        cJSON_AddItemToObject(env, AIRY_RS_K_DATA, data);
    }
    if (!cJSON_GetObjectItem(data, AIRY_RS_K_SUB_ID))
        cJSON_AddStringToObject(data, AIRY_RS_K_SUB_ID, c->sub_id);
    if (!cJSON_GetObjectItem(data, AIRY_RS_K_SUB_DEPTH))
        cJSON_AddNumberToObject(data, AIRY_RS_K_SUB_DEPTH, c->depth);
    c->up->emit(type, env, c->up->ud);
}

static void sub_ev(sub_ctx_t *c, const char *type, cJSON *data)
{
    if (!data)
        return;
    if (!c->own.emit) {
        cJSON_Delete(data);
        return;
    }
    agent_run_emit_event(&c->own, c->seq, c->run_id, c->sess_id, type, data);
}

static const char *sub_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(it) && it->valuestring && it->valuestring[0] ? it->valuestring : NULL;
}

/* 子任务提示词：策略面前缀 + 目标 + 调用方给出的背景。子代理看不到父会话
 * 历史，背景缺失就是背景缺失，不做隐式补全。 */
static char *sub_prompt(const char *goal, const char *context)
{
    const char *lead = sub_lead();
    size_t cap = strlen(lead) + strlen(goal) + (context ? strlen(context) : 0) + 32;
    char *p = (char *)AIRY_MALLOC(cap);
    if (!p)
        return NULL;
    if (context)
        snprintf(p, cap, "%s目标：%s\n\n上下文：\n%s", lead, goal, context);
    else
        snprintf(p, cap, "%s目标：%s", lead, goal);
    return p;
}

/* 预算：调用方显式给值就用它；否则取父预算的一份，低于下限按下限——子
 * 任务连一次工具往返都跑不完的预算等于没给。 */
static uint64_t sub_budget(const cJSON *args, uint64_t cap)
{
    const cJSON *b = cJSON_GetObjectItem(args, "budget");
    uint64_t share;
    if (cJSON_IsNumber(b) && b->valuedouble > 0.0 && b->valuedouble < 1e18)
        return (uint64_t)b->valuedouble;
    if (cap == 0)
        return AGENT_SUB_BUDGET_MIN;
    share = cap / AGENT_SUB_BUDGET_DIV;
    return share < AGENT_SUB_BUDGET_MIN ? AGENT_SUB_BUDGET_MIN : share;
}

static const char *sub_stop_of(int rc, const agent_run_loop_result_t *res)
{
    if (res->stop_reason)
        return res->stop_reason;
    return rc == AIRY_EBUDGET ? AIRY_RS_REASON_BUDGET : AIRY_RS_STOP_DONE;
}

/* 子 run 本体：续跑由工具循环自己的预算与续跑令牌承担，委派层只跑一轮。 */
static airy_err_t sub_exec(const char *tool, const char *args, size_t alen, char **out,
                           size_t *out_len, void *ud)
{
    sub_ctx_t *c = (sub_ctx_t *)ud;
    agent_run_loop_args_t la;
    agent_run_loop_result_t res;
    int rc;
    (void)tool;
    (void)alen;

    AIRY_MEMSET(&la, 0, sizeof(la));
    AIRY_MEMSET(&res, 0, sizeof(res));
    la.prompt = args;
    la.model = c->model;
    la.session = c->session;
    la.sink = c->own.emit ? &c->own : NULL;
    la.rounds = AGENT_RUN_ROUNDS_AUTO;
    la.token_budget = c->budget;
    la.allow_dangerous = (bool)(c->allow_dangerous != 0);
    la.agent_id = c->agent_id;
    la.seq_ext = c->seq;

    rc = agent_run_tool_loop(&la, &res);
    c->tokens += res.tokens;
    c->tokens_in += res.tokens_in;
    c->tokens_out += res.tokens_out;
    c->llm_ms += res.llm_ms;
    c->tool_ms += res.tool_ms;
    c->cost += res.cost;
    c->stop = sub_stop_of(rc, &res);
    if (res.trace)
        cJSON_Delete(res.trace);
    AIRY_FREE(res.reason);
    AIRY_FREE(res.resume_token);

    if (rc == 0 || rc == AIRY_EBUDGET) {
        if (res.text && res.text[0]) {
            *out = res.text;
            *out_len = strlen(res.text);
            return AIRY_SUCCESS;
        }
        AIRY_FREE(res.text);
        return AIRY_SUCCESS;
    }
    AIRY_FREE(res.text);
    if (rc == 1) {
        /* 取消必须落到任务态：只返回错误码会让 collect 看到假终态（FAILED），
         * 上层就分不清「用户取消」与「子任务失败」。 */
        if (c->task)
            airy_delegate_cancel(c->task);
        return AIRY_ECANCELLED;
    }
    return AIRY_ERR_UNKNOWN;
}

/* 深度/并发配额的拒绝话术必须带生效值：模型据此知道原地收敛还是等待，
 * 运维据此判断是配置过小还是任务拆分过碎。 */
static char *sub_deny(int depth_hit)
{
    agent_sub_limits_t lim = sub_limits();
    if (depth_hit)
        return sub_err("Error: 已达委派深度上限，请在本层自行完成子目标（max_depth=%d）",
                       lim.max_depth);
    return sub_err("Error: 并行子任务已满，请改为串行推进（max_concurrent=%d）", lim.max_conc);
}

int sub_task_run(const agent_sub_req_t *q, agent_sub_out_t *out)
{
    sub_ctx_t c;
    airy_delegate_config_t cfg;
    cJSON *args = NULL;
    const char *raw_goal = NULL;
    char *goal = NULL;
    char *prompt = NULL;
    char *text = NULL;
    cJSON *ev;
    airy_err_t derr;

    if (!q || !out)
        return AIRY_EINVAL;
    AIRY_MEMSET(out, 0, sizeof(*out));
    AIRY_MEMSET(&c, 0, sizeof(c));

    args = cJSON_Parse(q->args && q->args[0] ? q->args : "{}");
    if (!args) {
        out->text = sub_err("Error: %s 参数不是合法 JSON（code: %d）", AGENT_SUB_TOOL, AIRY_EINVAL);
        return AIRY_EINVAL;
    }
    raw_goal = sub_str(args, "goal");
    if (raw_goal)
        goal = AIRY_STRDUP(raw_goal);
    if (goal)
        prompt = sub_prompt(goal, sub_str(args, "context"));
    if (goal)
        c.budget = sub_budget(args, q->token_cap);
    cJSON_Delete(args);
    if (!goal || !prompt) {
        AIRY_FREE(goal);
        AIRY_FREE(prompt);
        out->text = sub_err("Error: %s 缺少 goal 或分配失败（code: %d）", AGENT_SUB_TOOL,
                            AIRY_ERR_OUT_OF_MEMORY);
        return AIRY_EBUSY;
    }

    if (sub_enter()) {
        out->text = sub_deny(1);
        AIRY_FREE(goal);
        AIRY_FREE(prompt);
        return AIRY_EBUSY;
    }
    if (!sub_slot_try()) {
        out->text = sub_deny(0);
        sub_leave();
        AIRY_FREE(goal);
        AIRY_FREE(prompt);
        return AIRY_EBUSY;
    }

    snprintf(c.sub_id, sizeof(c.sub_id), "sub_%llu",
             (unsigned long long)airy_atomic_fetch_add(&g_sub_seq, 1));
    c.session = q->session;
    c.up = q->sink;
    c.seq = q->seq ? q->seq : &c.local_seq;
    c.model = q->model;
    c.run_id = q->run_id;
    c.sess_id = q->sess_id;
    c.agent_id = q->agent_id;
    c.allow_dangerous = q->allow_dangerous;
    c.depth = sub_depth();
    c.stop = AIRY_RS_STOP_DONE;
    c.own.emit = sub_emit;
    c.own.ud = &c;

    ev = cJSON_CreateObject();
    if (ev) {
        cJSON_AddStringToObject(ev, AIRY_RS_K_SUB_GOAL, goal);
        cJSON_AddNumberToObject(ev, AIRY_RS_K_SUB_DEPTH, c.depth);
        cJSON_AddNumberToObject(ev, AIRY_RS_K_SUB_BUDGET, (double)c.budget);
        sub_ev(&c, AIRY_RS_TYPE_SUBAGENT_START, ev);
    }

    AIRY_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.focus_prompt = goal;
    cfg.max_depth = sub_limits().max_depth;
    cfg.max_iterations = 1;
    cfg.token_budget_ratio = 0.0f;
    c.task = airy_delegate_create(prompt, &cfg);
    if (!c.task) {
        derr = AIRY_ERR_OUT_OF_MEMORY;
    } else {
        derr = airy_delegate_assign(c.task, sub_exec, &c);
        airy_delegate_collect(c.task, &text, NULL);
        airy_delegate_destroy(c.task);
        c.task = NULL;
    }

    ev = cJSON_CreateObject();
    if (ev) {
        /* 失败不是取消：区分终态让客户端知道该换个目标还是该重连。 */
        const char *st = derr == AIRY_SUCCESS
                              ? c.stop
                              : (derr == AIRY_ECANCELLED ? AIRY_RS_STOP_CANCELLED
                                                         : AIRY_RS_REASON_AGENT);
        cJSON_AddStringToObject(ev, AIRY_RS_K_STATUS, st);
        cJSON_AddNumberToObject(ev, AIRY_RS_K_SUB_TOKENS, (double)c.tokens);
        sub_ev(&c, AIRY_RS_TYPE_SUBAGENT_END, ev);
    }

    out->tokens = c.tokens;
    out->tokens_in = c.tokens_in;
    out->tokens_out = c.tokens_out;
    out->llm_ms = c.llm_ms;
    out->tool_ms = c.tool_ms;
    out->cost = c.cost;
    out->text = text;
    if (!out->text) {
        /* 空结论不是成功：工具层「成功但无产出」会让模型以为拿到了答案。 */
        if (derr == AIRY_SUCCESS)
            derr = AIRY_ERR_UNKNOWN;
        out->text = sub_err("Error: %s 子任务未产出结论（code: %d）", AGENT_SUB_TOOL, (int)derr);
    }

    sub_slot_drop();
    sub_leave();
    AIRY_FREE(goal);
    AIRY_FREE(prompt);

    if (derr == AIRY_ECANCELLED)
        return AIRY_ECANCELLED;
    return derr == AIRY_SUCCESS ? 0 : AIRY_EBUSY;
}
