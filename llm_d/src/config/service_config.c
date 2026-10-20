// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_config.c
 * @brief LLM service config core: extension dispatch, pricing-rule release
 *        and the model-config loader.
 *
 * 2026-08-27 域拆分收尾（原主文件 → 仅保留分发与 JSON 定价核心）：
 *   - service_config_json.c           遗留 JSON 模型配置加载
 *   - service_config_yaml_models.c    models 行解析 + 简化 llm 段展开
 *   - service_config_yaml_providers.c provider 聚合导出
 *   - service_config_yaml_pricing.c   YAML 定价规则提取
 * 共享符号经 config/config_internal.h 声明。
 */

#include "airy_memory.h"
#include "daemon_defaults.h"
#include "error.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config_internal.h"
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
        return svc_load_model_config_yaml(config_path, out_providers, out_count);
    }
    return svc_load_model_config_json(config_path, out_providers, out_count);
}
