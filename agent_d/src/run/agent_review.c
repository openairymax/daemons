/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_review.c
 * @brief 语义复核挂载点：一次 think.review（pipeline 档）取回整链复核判定。
 *
 * 职责边界：本文件只做「构造入参 → RPC → 取判定」的搬运，不解析产物语义、
 * 不持有提示词、不链接复核机制库。管线编排与降级链在 coreloopthree
 * （execution_review），由 think_d 在自身进程内驱动；判断策略在
 * think_d/review_svc.c。think_d 不可达时按「复核不可用」返回 SKIP，不改变
 * 调用方既有结果。
 */

#include "agent_review.h"

#include "airy_memory.h"
#include "daemon_rpc_client.h"
#include "error.h"
#include "platform_paths.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

#define ARV_SOCK "think.sock"
/* 单次预算须覆盖门禁 + 两档模型预算，否则复核未完成就被调用方掐断。 */
#define ARV_MS 180000

/* 判定取值并搬运归因：think.review pipeline 档返回 {verdict, reason}。 */
static void arv_take(const cJSON *res, agent_review_result_t *out)
{
    if (!cJSON_IsObject(res))
        return;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(res, "verdict");
    if (cJSON_IsNumber(v) && v->valuedouble >= AIRY_REVIEW_PASS &&
        v->valuedouble <= AIRY_REVIEW_SKIP)
        out->verdict = (int)v->valuedouble;
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(res, "reason");
    if (cJSON_IsString(r) && r->valuestring)
        snprintf(out->reason, sizeof(out->reason), "%s", r->valuestring);
}

int agent_rev_apply(const char *goal, const char *body, const char *artifact_json,
                    const char *facts_json, agent_review_result_t *out)
{
    if (!out)
        return -1;
    out->verdict = AIRY_REVIEW_SKIP;
    out->reason[0] = '\0';
    if (!body || !body[0])
        return -1;

    cJSON *p = cJSON_CreateObject();
    if (!p)
        return -1;
    cJSON_AddStringToObject(p, "mode", "pipeline");
    if (goal)
        cJSON_AddStringToObject(p, "node_goal", goal);
    cJSON_AddStringToObject(p, "output_body", body);
    cJSON_AddStringToObject(p, "output_json", artifact_json ? artifact_json : body);
    if (facts_json)
        cJSON_AddStringToObject(p, "facts_json", facts_json);
    char *req = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    if (!req)
        return -1;

    char *resp = NULL;
    int rc = daemon_rpc_call(airy_runtime_dir_socket(ARV_SOCK), "review", req, &resp, ARV_MS);
    AIRY_FREE(req);
    if (rc != AIRY_SUCCESS || !resp) {
        SVC_LOG_WARN("agent.review: think.review unreachable, keep run result");
        AIRY_FREE(resp);
        return 0;
    }

    cJSON *res = cJSON_Parse(resp);
    AIRY_FREE(resp);
    arv_take(res, out);
    cJSON_Delete(res);
    return 0;
}
