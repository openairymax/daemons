// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file review_svc.c
 * @brief think.review 执行复核服务面（M1-1c）。
 *
 * CLI 原进程内承载 t2/t1-f 语义复核（提示词 + LLM 判断，0.1.9 方案
 * §2.3 判定为执行环策略直连）。本模块将复核策略收拢到 think_d 服务面，
 * CLI 经 gateway 调用 think.review，满足"策略迁 daemon、机制留 core"。
 *
 * execution_review 机制本体（gate→t2→t1-f 编排）留 coreloopthree 供
 * work_hall 复核回调使用；本 daemon 用同一机制承载执行中复核（本文件
 * pipeline 档），并对外只暴露语义判断（t2 / t1-f 档）。模型路由取 think_d
 * 已配置的 GRAD 角色模型：t2 用 think2_slow_model（A 角色），t1-f 用
 * think1_fast_model（B 角色）；未显式配置时返回 verdict=-1，由机制层走
 * 既定降级（gate-only / adopt t2），杜绝与主生成同模型自审自签。
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "execution_review.h"
#include "jsonrpc_helpers.h"
#include "llm_svc_adapter.h"
#include "llm_service.h"
#include "review_svc.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

#define REVIEW_MAX_TOKENS 128
#define REVIEW_PROMPT_MAX 6144
#define REVIEW_REASON_MAX 128

/* t2 (A) 语义审查：artifact 产物 vs 蓝图节点契约。 */
static const char REVIEW_T2_SYSTEM[] =
    "你是 AgentRT 执行中复核的 t2 语义审查器。判断执行产物是否满足蓝图节点的目标"
    "node_goal。必须先阅读 output_body 的正文本身再下结论；output_json 只是结构字段，"
    "不得只凭结构字段裁决。注意：output_signatures 中形如 artifact:xxx 的键是 GRAD "
    "因果引用签名（描述图上下游因果链接），不是产物必须包含的内容键，不要据此判定偏离。"
    "只输出一行 JSON：{\"drift\":0或1,\"reason\":\"不超过60字\"}。drift=1 表示产物偏离"
    "目标或存在明显缺陷，drift=0 表示产物满足目标。";

/* t1-f (B) 终裁：综合确定性门禁与 t2 偏离结论。 */
static const char REVIEW_T1F_SYSTEM[] =
    "你是 AgentRT 执行中复核的 t1-f 终裁者。必须先阅读 output_body 的正文本身，"
    "再综合确定性门禁结果 gate_reason 与 t2 语义偏离结论 drift，对执行产物做出最终"
    "接受/拒绝决定；output_json 只是结构字段，不得只凭结构字段裁决。"
    "只输出一行 JSON：{\"accept\":0或1,\"reason\":\"不超过60字\"}。"
    "accept=0 表示拒绝（产物不合格，需要重做或人工介入），accept=1 表示接受。";

static llm_svc_adapter_t *g_adapter = NULL;
static const char *g_t2_model = NULL;
static const char *g_t1f_model = NULL;

int review_svc_init(llm_svc_adapter_t *adapter, const char *t2_model,
                    const char *t1f_model)
{
    g_adapter = adapter;
    g_t2_model = t2_model;
    g_t1f_model = t1f_model;
    return 0;
}

void review_svc_cleanup(void)
{
    g_adapter = NULL;
    g_t2_model = NULL;
    g_t1f_model = NULL;
}

/* 提取 JSON 串中的 "key":"..." 值（宽容：思考模型前缀推理文本也可用，
 * JSON 转义原样拷贝）。命中返回 1，否则 0。 */
static int review_json_str(const char *json, const char *key, char *buf, size_t sz)
{
    if (!json || !key || !buf || sz == 0)
        return 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < sz)
        buf[o++] = *p++;
    buf[o] = '\0';
    return o > 0;
}

/* warn 档兼容：旧式子串提取（仅 AIRY_REVIEW_SCHEMA=warn 时可达）。 */
static int rv_legacy_get(const char *content, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":1", key);
    if (strstr(content, pat))
        return 1;
    snprintf(pat, sizeof(pat), "\"%s\": 1", key);
    if (strstr(content, pat))
        return 1;
    snprintf(pat, sizeof(pat), "\"%s\":0", key);
    if (strstr(content, pat))
        return 0;
    snprintf(pat, sizeof(pat), "\"%s\": 0", key);
    if (strstr(content, pat))
        return 0;
    return -1;
}

/* 单轮 LLM 判断（system + user）。成功返回 content（OWNER），失败 NULL。 */
static char *review_llm(const char *sys, const char *user, const char *model)
{
    if (!g_adapter || !sys || !user || !model || !model[0])
        return NULL;

    llm_message_t msgs[2];
    __builtin_memset(&msgs, 0, sizeof(msgs));
    msgs[0].role = "system";
    msgs[0].content = sys;
    msgs[1].role = "user";
    msgs[1].content = user;

    llm_request_config_t cfg;
    __builtin_memset(&cfg, 0, sizeof(cfg));
    cfg.model = model;
    cfg.messages = msgs;
    cfg.message_count = 2;
    cfg.temperature = 0.0f;
    cfg.max_tokens = REVIEW_MAX_TOKENS;

    llm_response_t *resp = NULL;
    if (llm_svc_adapter_complete(g_adapter, &cfg, &resp) != 0 || !resp ||
        resp->choice_count == 0 || !resp->choices[0].content) {
        if (resp)
            llm_response_free(resp);
        return NULL;
    }
    char *out = AIRY_STRDUP(resp->choices[0].content);
    llm_response_free(resp);
    return out;
}

/* t2 语义复核：verdict=1 偏离 / 0 满足 / -1 无法判断（机制层降级）。 */
static int review_t2(const char *node_goal, const char *output_json,
                     const char *output_body, cJSON *sigs, char *reason_out,
                     size_t reason_sz)
{
    if (!g_t2_model) {
        SVC_LOG_WARN("review t2: model not configured, degrade to gate-only");
        return -1;
    }
    /* 只把真实内容契约键交给 LLM（artifact:* 引用签名过滤掉） */
    char sigbuf[256] = "(无)";
    if (sigs && cJSON_IsArray(sigs)) {
        size_t o = 0;
        int n = cJSON_GetArraySize(sigs);
        for (int i = 0; i < n && o < sizeof(sigbuf) - 2; i++) {
            cJSON *it = cJSON_GetArrayItem(sigs, i);
            if (!cJSON_IsString(it) || !it->valuestring || !it->valuestring[0] ||
                strncmp(it->valuestring, "artifact:", 9) == 0)
                continue;
            o += (size_t)snprintf(sigbuf + o, sizeof(sigbuf) - o, "%s%s", o ? "," : "",
                                  it->valuestring);
        }
        if (o == 0)
            snprintf(sigbuf, sizeof(sigbuf), "(无)");
    }

    char user[REVIEW_PROMPT_MAX];
    rv_prompt_t2(user, sizeof(user), node_goal, sigbuf, output_body, output_json);

    char *content = review_llm(REVIEW_T2_SYSTEM, user, g_t2_model);
    if (!content)
        return -1;
    int rc = -1;
    int raw = -1;
    char rb[REVIEW_REASON_MAX] = "";
    if (rv_schema_parse(content, "drift", &raw, rb, sizeof(rb))) {
        rc = raw;
    } else if (rv_schema_mode() == 1) {
        SVC_LOG_WARN("C-4: review t2 schema invalid, warn mode falls back");
        rc = rv_legacy_get(content, "drift");
        if (rc >= 0)
            review_json_str(content, "reason", rb, sizeof(rb));
    } else {
        SVC_LOG_WARN("C-4: review t2 schema invalid, verdict unavailable");
        rc = -1;
    }
    if (rc == 1 && reason_out && reason_sz > 0 && rb[0])
        snprintf(reason_out, reason_sz, "%s", rb);
    AIRY_FREE(content);
    return rc;
}

/* t1-f 终裁：verdict=0 接受 / 1 拒绝 / -1 无法判断（机制层 adopt t2）。 */
static int review_t1f(const char *gate_reason, int drift, const char *node_goal,
                      const char *output_json, const char *output_body,
                      char *reason_out, size_t reason_sz)
{
    if (!g_t1f_model) {
        SVC_LOG_WARN("review t1-f: model not configured, adopt t2 verdict");
        return -1;
    }

    char user[REVIEW_PROMPT_MAX];
    rv_prompt_t1f(user, sizeof(user), gate_reason, drift, node_goal, output_body,
                  output_json);

    char *content = review_llm(REVIEW_T1F_SYSTEM, user, g_t1f_model);
    if (!content)
        return -1;
    int rc = -1;
    int raw = -1;
    char rb[REVIEW_REASON_MAX] = "";
    if (rv_schema_parse(content, "accept", &raw, rb, sizeof(rb))) {
        rc = rv_accept_map(raw);
    } else if (rv_schema_mode() == 1) {
        SVC_LOG_WARN("C-4: review t1f schema invalid, warn mode falls back");
        int acc = rv_legacy_get(content, "accept");
        rc = (acc < 0) ? -1 : rv_accept_map(acc);
        if (rc >= 0)
            review_json_str(content, "reason", rb, sizeof(rb));
    } else {
        SVC_LOG_WARN("C-4: review t1f schema invalid, verdict unavailable");
        rc = -1;
    }
    if (rc >= 0 && reason_out && reason_sz > 0 && rb[0])
        snprintf(reason_out, reason_sz, "%s", rb);
    AIRY_FREE(content);
    return rc;
}

/* pipeline 档：调用方一次 RPC 让本 daemon 在进程内跑完整复核链
 * （确定性门禁 → t2 → t1-f）。执行体因此只持有挂载点，不必直挂
 * coreloopthree 机制库（G13 扇出纪律）。 */
typedef struct {
    const char *goal;
    const char *body;
    const char *json;
} rv_pipe_t;

static int rv_pipe_t2(const airy_review_input_t *in, const char *fact, char *reason,
                      size_t sz, void *ud)
{
    (void)in;
    (void)fact;
    const rv_pipe_t *p = (const rv_pipe_t *)ud;
    return review_t2(p->goal, p->json, p->body, NULL, reason, sz);
}

static int rv_pipe_t1f(const airy_review_input_t *in, int drift, const char *gate,
                       char *reason, size_t sz, void *ud)
{
    (void)in;
    const rv_pipe_t *p = (const rv_pipe_t *)ud;
    return review_t1f(gate, drift, p->goal, p->json, p->body, reason, sz);
}

/* 跑一次同步管线；返回判定值（-1 = 机制不可用，调用方保持既有结果）。 */
static int rv_pipeline(const char *goal, const char *body, const char *json,
                       const char *facts, char *reason, size_t sz)
{
    airy_review_config_t cfg;
    __builtin_memset(&cfg, 0, sizeof(cfg));
    cfg.fact_base_json = facts;
    cfg.require_output_nonempty = 1;

    airy_execution_review_t *rv = airy_execution_review_create(&cfg);
    if (!rv) {
        SVC_LOG_WARN("review pipeline: reviewer create failed");
        return -1;
    }

    rv_pipe_t p = {goal, body, json};
    airy_review_semantic_ops_t ops;
    __builtin_memset(&ops, 0, sizeof(ops));
    ops.t2_review = rv_pipe_t2;
    ops.t1f_adjudicate = rv_pipe_t1f;
    ops.user_data = &p;
    airy_execution_review_set_semantic_ops(rv, &ops);

    airy_review_input_t in;
    __builtin_memset(&in, 0, sizeof(in));
    in.node_goal = goal;
    in.output_json = json;

    airy_review_verdict_t v = AIRY_REVIEW_SKIP;
    airy_err_t e = airy_execution_review_run(rv, &in, &v, reason, sz);
    airy_execution_review_destroy(rv);
    if (e != AIRY_SUCCESS)
        return -1;
    return (int)v;
}

/* think.review：params {mode?: pipeline, stage?: t2|t1f, node_goal?,
 * output_json?, output_body?（C-4：产物正文，B 侧必读）, output_signatures?[],
 * gate_reason?(t1f), drift?(t1f), facts_json?(pipeline)}
 * result: {verdict: -1|0..3, reason} */
void review_svc_process(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *ng = cJSON_GetObjectItem(params, "node_goal");
    cJSON *oj = cJSON_GetObjectItem(params, "output_json");
    cJSON *ob = cJSON_GetObjectItem(params, "output_body");
    const char *node_goal = cJSON_IsString(ng) ? ng->valuestring : NULL;
    const char *output_json = cJSON_IsString(oj) ? oj->valuestring : NULL;
    const char *output_body = cJSON_IsString(ob) ? ob->valuestring : NULL;

    int verdict = -1;
    char reason[REVIEW_REASON_MAX] = "";
    cJSON *md = cJSON_GetObjectItem(params, "mode");
    if (cJSON_IsString(md) && md->valuestring && strcmp(md->valuestring, "pipeline") == 0) {
        cJSON *fj = cJSON_GetObjectItem(params, "facts_json");
        const char *facts = cJSON_IsString(fj) ? fj->valuestring : NULL;
        verdict = rv_pipeline(node_goal, output_body, output_json, facts, reason,
                              sizeof(reason));
    } else {
        cJSON *stage = cJSON_GetObjectItem(params, "stage");
        JSONRPC_REQUIRE(cJSON_IsString(stage) && stage->valuestring && stage->valuestring[0],
                        client_fd, "Missing stage string", id);
        if (strcmp(stage->valuestring, "t2") == 0) {
            cJSON *sigs = cJSON_GetObjectItem(params, "output_signatures");
            verdict = review_t2(node_goal, output_json, output_body, sigs, reason,
                                sizeof(reason));
        } else if (strcmp(stage->valuestring, "t1f") == 0) {
            cJSON *gr = cJSON_GetObjectItem(params, "gate_reason");
            cJSON *df = cJSON_GetObjectItem(params, "drift");
            const char *gate_reason = cJSON_IsString(gr) ? gr->valuestring : NULL;
            int drift = cJSON_IsNumber(df) ? (int)df->valuedouble : 0;
            verdict = review_t1f(gate_reason, drift, node_goal, output_json, output_body,
                                 reason, sizeof(reason));
        } else {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Unknown stage", id);
            return;
        }
    }

    cJSON *res = cJSON_CreateObject();
    if (!res) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddNumberToObject(res, "verdict", verdict);
    cJSON_AddStringToObject(res, "reason", reason);
    JSONRPC_SEND_SUCCESS(client_fd, res, id);
}
