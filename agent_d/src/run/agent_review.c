/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_review.c
 * @brief 语义复核挂载点：把 coreloopthree 复核管线的 t2 / t1-f 两档
 *        委托给 think_d 的 think.review 方法。
 *
 * 职责边界：本文件只做「构造入参 → RPC → 取判定」的搬运，不解析产物语义、
 * 不持有提示词；管线编排与降级链在 execution_review.c，判断策略在
 * think_d/review_svc.c。任一次 RPC 不可达按「档位不可用」返回 -1，由管线
 * 沿既有降级链收敛，不改变调用方既有结果。
 */

#include "agent_review.h"

#include "execution_review.h"

#include "airy_memory.h"
#include "daemon_rpc_client.h"
#include "error.h"
#include "platform_paths.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

#define ARV_SOCK "think.sock"
/* 单档预算须覆盖 think_d 的模型预算，否则复核未完成就被调用方掐断。 */
#define ARV_MS 90000
#define ARV_UNAVAIL (-1)

/* 复核入参在一次管线的两档之间共用，字段全部借用。 */
typedef struct {
    const char *goal;
    const char *body;
    const char *artifact;
} arv_ctx_t;

/* 单档调用 think.review；返回 result 对象（OWNER），不可达/坏载荷返回 NULL。 */
static cJSON *arv_rpc(const arv_ctx_t *c, const char *stage, int drift, const char *gate)
{
    cJSON *p = cJSON_CreateObject();
    if (!p)
        return NULL;
    cJSON_AddStringToObject(p, "stage", stage);
    if (c->goal)
        cJSON_AddStringToObject(p, "node_goal", c->goal);
    if (c->body)
        cJSON_AddStringToObject(p, "output_body", c->body);
    if (c->artifact)
        cJSON_AddStringToObject(p, "output_json", c->artifact);
    if (strcmp(stage, "t1f") == 0) {
        cJSON_AddNumberToObject(p, "drift", drift);
        if (gate)
            cJSON_AddStringToObject(p, "gate_reason", gate);
    }
    char *req = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    if (!req)
        return NULL;

    char *resp = NULL;
    int rc = daemon_rpc_call(airy_runtime_dir_socket(ARV_SOCK), "review", req, &resp, ARV_MS);
    AIRY_FREE(req);
    if (rc != AIRY_SUCCESS || !resp) {
        AIRY_FREE(resp);
        return NULL;
    }
    cJSON *out = cJSON_Parse(resp);
    AIRY_FREE(resp);
    return out;
}

/* 判定取值并搬运归因：think.review 统一返回 {verdict, reason}。 */
static int arv_take(cJSON *res, char *reason, size_t sz)
{
    if (!res)
        return ARV_UNAVAIL;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(res, "verdict");
    int rc = cJSON_IsNumber(v) ? (int)v->valuedouble : ARV_UNAVAIL;
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(res, "reason");
    if (reason && sz > 0 && cJSON_IsString(r) && r->valuestring)
        snprintf(reason, sz, "%s", r->valuestring);
    cJSON_Delete(res);
    return rc;
}

static int arv_t2(const airy_review_input_t *in, const char *fact, char *reason, size_t sz,
                  void *ud)
{
    (void)in;
    (void)fact;
    return arv_take(arv_rpc((const arv_ctx_t *)ud, "t2", 0, NULL), reason, sz);
}

static int arv_t1f(const airy_review_input_t *in, int drift, const char *gate, char *reason,
                   size_t sz, void *ud)
{
    (void)in;
    return arv_take(arv_rpc((const arv_ctx_t *)ud, "t1f", drift, gate), reason, sz);
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

    airy_review_config_t cfg;
    AIRY_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.fact_base_json = facts_json;
    cfg.require_output_nonempty = 1;

    airy_execution_review_t *rv = airy_execution_review_create(&cfg);
    if (!rv) {
        SVC_LOG_WARN("agent.review: reviewer create failed, skip semantic review");
        return -1;
    }

    arv_ctx_t ctx = {goal, body, artifact_json ? artifact_json : body};
    airy_review_semantic_ops_t ops;
    AIRY_MEMSET(&ops, 0, sizeof(ops));
    ops.t2_review = arv_t2;
    ops.t1f_adjudicate = arv_t1f;
    ops.user_data = &ctx;
    airy_execution_review_set_semantic_ops(rv, &ops);

    airy_review_input_t in;
    AIRY_MEMSET(&in, 0, sizeof(in));
    in.node_goal = goal;
    in.output_json = ctx.artifact;

    airy_review_verdict_t v = AIRY_REVIEW_SKIP;
    airy_err_t e = airy_execution_review_run(rv, &in, &v, out->reason, sizeof(out->reason));
    airy_execution_review_destroy(rv);
    if (e != AIRY_SUCCESS)
        return -1;
    out->verdict = (int)v;
    return 0;
}
