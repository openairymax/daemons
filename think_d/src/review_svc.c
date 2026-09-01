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
 * work_hall 复核回调使用；此处仅提供语义判断（提示词工程 + 模型路由 +
 * LLM 调用）。模型路由取 think_d 已配置的 GRAD 角色模型：t2 用
 * think2_slow_model（A 角色），t1-f 用 think1_fast_model（B 角色）；
 * 未显式配置时返回 verdict=-1，由机制层走既定降级（gate-only / adopt
 * t2），杜绝与主生成同模型自审自签。
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "jsonrpc_helpers.h"
#include "llm_svc_adapter.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <string.h>

#define REVIEW_MAX_TOKENS 128
#define REVIEW_PROMPT_MAX 4096
#define REVIEW_REASON_MAX 128

/* t2 (A) 语义审查：artifact 产物 vs 蓝图节点契约。 */
static const char REVIEW_T2_SYSTEM[] =
    "你是 AgentRT 执行中复核的 t2 语义审查器。判断执行产物 output_json 是否满足"
    "蓝图节点的目标 node_goal。注意：output_signatures 中形如 artifact:xxx 的键"
    "是 GRAD 因果引用签名（描述图上下游因果链接），不是产物必须包含的内容键，"
    "不要据此判定偏离；请以 node_goal 达成度为主要判据。只输出一行 JSON："
    "{\"drift\":0或1,\"reason\":\"不超过60字\"}。drift=1 表示产物偏离目标或存在"
    "明显缺陷，drift=0 表示产物满足目标。";

/* t1-f (B) 终裁：综合确定性门禁与 t2 偏离结论。 */
static const char REVIEW_T1F_SYSTEM[] =
    "你是 AgentRT 执行中复核的 t1-f 终裁者。综合确定性门禁结果 gate_reason 与 t2 "
    "语义偏离结论 drift，对执行产物 output_json 做出最终接受/拒绝决定。"
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
static int review_t2(const char *node_goal, const char *output_json, cJSON *sigs,
                     char *reason_out, size_t reason_sz)
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
    snprintf(user, sizeof(user), "node_goal: %s\noutput_signatures: %s\noutput_json: %s",
             node_goal ? node_goal : "(无)", sigbuf, output_json ? output_json : "(无)");

    char *content = review_llm(REVIEW_T2_SYSTEM, user, g_t2_model);
    if (!content)
        return -1;
    int rc = -1;
    if (strstr(content, "\"drift\":1") || strstr(content, "\"drift\": 1"))
        rc = 1;
    else if (strstr(content, "\"drift\":0") || strstr(content, "\"drift\": 0"))
        rc = 0;
    if (rc == 1 && reason_out && reason_sz > 0) {
        char rb[REVIEW_REASON_MAX];
        if (review_json_str(content, "reason", rb, sizeof(rb)))
            snprintf(reason_out, reason_sz, "%s", rb);
    }
    AIRY_FREE(content);
    return rc;
}

/* t1-f 终裁：verdict=0 接受 / 1 拒绝 / -1 无法判断（机制层 adopt t2）。 */
static int review_t1f(const char *gate_reason, int drift, const char *node_goal,
                      const char *output_json, char *reason_out, size_t reason_sz)
{
    if (!g_t1f_model) {
        SVC_LOG_WARN("review t1-f: model not configured, adopt t2 verdict");
        return -1;
    }

    char user[REVIEW_PROMPT_MAX];
    snprintf(user, sizeof(user), "gate_reason: %s\ndrift: %d\nnode_goal: %s\noutput_json: %s",
             (gate_reason && gate_reason[0]) ? gate_reason : "gate ok", drift,
             node_goal ? node_goal : "(无)", output_json ? output_json : "(无)");

    char *content = review_llm(REVIEW_T1F_SYSTEM, user, g_t1f_model);
    if (!content)
        return -1;
    int rc = -1;
    if (strstr(content, "\"accept\":0") || strstr(content, "\"accept\": 0"))
        rc = 1; /* reject */
    else if (strstr(content, "\"accept\":1") || strstr(content, "\"accept\": 1"))
        rc = 0; /* accept */
    if (rc >= 0 && reason_out && reason_sz > 0) {
        char rb[REVIEW_REASON_MAX];
        if (review_json_str(content, "reason", rb, sizeof(rb)) && rb[0])
            snprintf(reason_out, reason_sz, "%s", rb);
    }
    AIRY_FREE(content);
    return rc;
}

/* think.review：params {stage: t2|t1f, node_goal?, output_json?,
 * output_signatures?[], gate_reason?(t1f), drift?(t1f)}
 * result: {verdict: -1|0|1, reason} */
void review_svc_process(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *stage = cJSON_GetObjectItem(params, "stage");
    if (!cJSON_IsString(stage) || !stage->valuestring || !stage->valuestring[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing stage string", id);
        return;
    }
    cJSON *ng = cJSON_GetObjectItem(params, "node_goal");
    cJSON *oj = cJSON_GetObjectItem(params, "output_json");
    const char *node_goal = cJSON_IsString(ng) ? ng->valuestring : NULL;
    const char *output_json = cJSON_IsString(oj) ? oj->valuestring : NULL;

    int verdict = -1;
    char reason[REVIEW_REASON_MAX] = "";
    if (strcmp(stage->valuestring, "t2") == 0) {
        cJSON *sigs = cJSON_GetObjectItem(params, "output_signatures");
        verdict = review_t2(node_goal, output_json, sigs, reason, sizeof(reason));
    } else if (strcmp(stage->valuestring, "t1f") == 0) {
        cJSON *gr = cJSON_GetObjectItem(params, "gate_reason");
        cJSON *df = cJSON_GetObjectItem(params, "drift");
        const char *gate_reason = cJSON_IsString(gr) ? gr->valuestring : NULL;
        int drift = cJSON_IsNumber(df) ? (int)df->valuedouble : 0;
        verdict = review_t1f(gate_reason, drift, node_goal, output_json, reason,
                             sizeof(reason));
    } else {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Unknown stage", id);
        return;
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
