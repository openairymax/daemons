// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_metrics.c
 * @brief LLM service statistics domain: input-complexity evaluation,
 *        routing-decision audit logs, service-stats export and
 *        model-list/default-model queries.
 */

#include "airy_memory.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <string.h>

#include "llm_service_internal.h"

/**
 * @brief Assess complexity from the input text (BAN-133: SIMPLE/MODERATE/COMPLEX)
 *
 * Scoring rules:
 *   - input length > 500 chars +2
 *   - input length > 100 chars +1
 *   - contains architecture/design/system-level keywords +1
 *   - contains multi-step markers +2
 *   - contains code-generation markers +1
 *
 * Routing:
 *   - SIMPLE   (0-1):  gpt-4o-mini
 *   - MODERATE (2-4):  gpt-4o
 *   - COMPLEX  (5+):   claude-sonnet
 */
llm_complexity_level_t assess_complexity(const char *input)
{
    if (!input)
        return LLM_COMPLEXITY_SIMPLE;

    size_t len = strlen(input);
    int score = 0;

    if (len > 500)
        score += 2;
    else if (len > 100)
        score += 1;

    const char *complex_kw[] = {"architecture", "distributed", "system design", "scalability",
                                "架构",         "分布式",      "系统设计",      "高可用",
                                "微服务",       "重构"};
    for (size_t i = 0; i < sizeof(complex_kw) / sizeof(complex_kw[0]); i++) {
        if (strstr(input, complex_kw[i])) {
            score += 1;
            break;
        }
    }

    const char *multi_step_kw[] = {"first", "then", "finally", "step 1", "step 2",
                                   "首先",  "然后", "最后",    "第一步", "第二步"};
    for (size_t i = 0; i < sizeof(multi_step_kw) / sizeof(multi_step_kw[0]); i++) {
        if (strstr(input, multi_step_kw[i])) {
            score += 2;
            break;
        }
    }

    const char *code_kw[] = {"function", "algorithm", "implement", "write a", "函数",
                             "算法",     "实现",      "编写",      "代码"};
    for (size_t i = 0; i < sizeof(code_kw) / sizeof(code_kw[0]); i++) {
        if (strstr(input, code_kw[i])) {
            score += 1;
            break;
        }
    }

    if (score >= 5)
        return LLM_COMPLEXITY_COMPLEX;
    if (score >= 2)
        return LLM_COMPLEXITY_MODERATE;
    return LLM_COMPLEXITY_SIMPLE;
}

/**
 * @brief Pick the default model by complexity (BAN-133 coding contract)
 */
static __attribute__((unused)) const char *route_by_complexity(llm_complexity_level_t level)
{
    switch (level) {
    case LLM_COMPLEXITY_SIMPLE:
        return "gpt-4o-mini";
    case LLM_COMPLEXITY_MODERATE:
        return "gpt-4o";
    case LLM_COMPLEXITY_COMPLEX:
        return "claude-sonnet";
    default:
        return "gpt-4o-mini";
    }
}

/**
 * @brief Record the routing-decision audit log (BAN-137 coding contract)
 */
void log_routing_decision(const char *model, llm_complexity_level_t complexity,
                          size_t input_len, const char *reason)
{
    const char *complexity_names[] = {"SIMPLE", "MODERATE", "COMPLEX"};
    SVC_LOG_INFO("[ROUTING] model=%s complexity=%s input_len=%zu reason=%s",
                 model ? model : "unknown", complexity_names[complexity], input_len,
                 reason ? reason : "default");
}

int llm_service_stats(llm_service_t *svc, char **out_json)
{
    if (!svc || !out_json) {
        SVC_LOG_ERROR("llm_service_stats: NULL parameter (svc=%p, out_json=%p)", (const void *)svc,
                      (const void *)out_json);
        return AIRY_ERR_INVALID_PARAM;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        SVC_LOG_ERROR("llm_service_stats: cJSON_CreateObject failed");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    cJSON *cost_json = cost_tracker_export(svc->cost);
    if (cost_json) {
        cJSON_AddItemToObject(root, "cost", cost_json);
    }

    cJSON_AddNumberToObject(root, "llm_cache_size", llm_cache_size(svc->cache));
    cJSON_AddNumberToObject(root, "llm_cache_capacity", llm_cache_capacity(svc->cache));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        SVC_LOG_ERROR("llm_service_stats: cJSON_PrintUnformatted failed");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    *out_json = json;
    return AIRY_OK;
}

typedef struct {
    cJSON *models_arr;
    const char *default_model;
} list_models_ctx_t;

static int list_models_cb(const char *provider_name, const char *model_name, void *user_data)
{
    list_models_ctx_t *ctx = (list_models_ctx_t *)user_data;
    if (!ctx || !ctx->models_arr || !provider_name || !model_name)
        return 0;

    cJSON *item = cJSON_CreateObject();
    if (!item)
        return 0;
    cJSON_AddStringToObject(item, "name", model_name);
    cJSON_AddStringToObject(item, "provider", provider_name);
    cJSON_AddBoolToObject(item, "default",
                          ctx->default_model && strcmp(ctx->default_model, model_name) == 0);
    cJSON_AddItemToArray(ctx->models_arr, item);
    return 0;
}

char *llm_service_list_models(llm_service_t *svc)
{
    if (!svc)
        return NULL;

    airy_mtx_lock(&svc->lock);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        airy_mtx_unlock(&svc->lock);
        return NULL;
    }
    cJSON *models_arr = cJSON_CreateArray();
    if (!models_arr) {
        cJSON_Delete(root);
        airy_mtx_unlock(&svc->lock);
        return NULL;
    }
    cJSON_AddItemToObject(root, "models", models_arr);

    list_models_ctx_t ctx = {
        .models_arr = models_arr,
        .default_model = svc->default_model[0] ? svc->default_model : NULL,
    };
    provider_registry_enumerate(svc->registry, list_models_cb, &ctx);

    cJSON_AddStringToObject(root, "default_model", svc->default_model[0] ? svc->default_model : "");
    if (svc->default_provider[0])
        cJSON_AddStringToObject(root, "default_provider", svc->default_provider);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    airy_mtx_unlock(&svc->lock);
    return json;
}

const char *llm_service_default_model(const llm_service_t *svc)
{
    if (!svc)
        return NULL;
    return svc->default_model[0] ? svc->default_model : NULL;
}
