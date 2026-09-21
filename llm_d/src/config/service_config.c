// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_config.c
 * @brief LLM service config core: extension dispatch, JSON pricing-rule
 *        parsing/release and the model-config loader.
 *
 * 2026-08-27 域拆分收尾（原主文件 → 仅保留分发与 JSON 定价核心）：
 *   - service_config_json.c           JSON 模型配置加载
 *   - service_config_yaml_models.c    models 行解析 + 简化 llm 段展开
 *   - service_config_yaml_providers.c provider 聚合导出
 *   - service_config_yaml_pricing.c   YAML 定价规则提取
 * 共享符号经 config/internal.h 声明；YAML 函数体仅开启 HAVE_YAML
 * 时编译。
 */

#include "airy_memory.h"
#include "daemon_defaults.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/internal.h"
#include "config/types.h"

int ends_with(const char *str, const char *suffix)
{
    if (!str || !suffix)
        return 0;
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len)
        return 0;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

/**
 * @brief Load pricing rules
 * @param root  JSON root node
 * @param count Output rule count
 * @return Rule array (caller frees), NULL on failure
 */
pricing_rule_t *load_pricing_rules(cJSON *root, int *count)
{
    if (!root || !count) {
        SVC_LOG_ERROR("load_pricing_rules: NULL parameter (root=%p, count=%p)", (const void *)root,
                      (const void *)count);
        *count = 0;
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    cJSON *pricing = cJSON_GetObjectItem(root, "pricing");
    if (!pricing || !cJSON_IsArray(pricing)) {
        SVC_LOG_ERROR("load_pricing_rules: pricing array missing or not an array in config");
        *count = 0;
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    int n = cJSON_GetArraySize(pricing);
    pricing_rule_t *rules = (pricing_rule_t *)AIRY_CALLOC((size_t)n, sizeof(pricing_rule_t));
    if (!rules) {
        SVC_LOG_ERROR("load_pricing_rules: calloc failed for %d pricing rules", n);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    for (int i = 0; i < n; ++i) {
        cJSON *item = cJSON_GetArrayItem(pricing, i);
        cJSON *pattern = cJSON_GetObjectItem(item, "pattern");
        cJSON *input = cJSON_GetObjectItem(item, "input_price_per_k");
        cJSON *output = cJSON_GetObjectItem(item, "output_price_per_k");

        if (cJSON_IsString(pattern) && cJSON_IsNumber(input) && cJSON_IsNumber(output)) {
            rules[i].model_pattern = AIRY_STRDUP(pattern->valuestring);
            if (!rules[i].model_pattern) {

                SVC_LOG_ERROR("load_pricing_rules: strdup failed for model_pattern at index %d", i);
                for (int j = 0; j < i; ++j) {
                    AIRY_FREE((void *)rules[j].model_pattern);
                }
                AIRY_FREE(rules);
                *count = 0;
                AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
            }
            rules[i].input_price_per_k = input->valuedouble;
            rules[i].output_price_per_k = output->valuedouble;
        } else {
            rules[i].model_pattern = NULL;
        }
    }

    *count = n;
    return rules;
}

void free_pricing_rules(pricing_rule_t *rules, int count)
{
    if (!rules)
        return;
    for (int i = 0; i < count; ++i) {
        AIRY_FREE((void *)rules[i].model_pattern);
    }
    AIRY_FREE(rules);
}

int svc_load_model_config(const char *config_path, provider_config_t **out_providers,
                          size_t *out_count)
{
    if (!config_path || !out_providers || !out_count) {
        SVC_LOG_ERROR("C-L02: SVC: MODEL-CONFIG-FAIL NULL parameter, STACK: svc_load_model_config");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (ends_with(config_path, ".yaml") || ends_with(config_path, ".yml")) {
#ifdef HAVE_YAML
        return svc_load_model_config_yaml(config_path, out_providers, out_count);
#else
        SVC_LOG_ERROR(
            "C-L02: SVC: MODEL-CONFIG-FAIL YAML not compiled, STACK: svc_load_model_config");
        return AIRY_ERR_NOT_SUPPORTED;
#endif
    }
    return svc_load_model_config_json(config_path, out_providers, out_count);
}
