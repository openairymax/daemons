// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_config_yaml.c
 * @brief LLM config YAML parsing infrastructure (split from
 *        service_config.c, 2026-08-27): flat key/value map shared by the
 *        models / providers / pricing YAML loaders.
 *
 * 2026-08-27 域拆分（service_config.c 剩余 1039 行 → 5 文件）：
 *   - service_config.c                非 YAML 核心（ends_with / JSON 定价 /
 *                                     模型配置分发）
 *   - service_config_yaml.c           YAML 基础设施（本文件）：kv map
 *   - service_config_yaml_models.c    models 状态机 + 简化 llm 段展开
 *   - service_config_yaml_providers.c provider 聚合导出
 *   - service_config_yaml_pricing.c   YAML 定价规则提取
 *
 * kv map 与解析状态结构经 config/types.h 共享，
 * 全部 YAML 函数体仅在开启 HAVE_YAML 时编译。
 */

#include "airy_memory.h"
#include "daemon_defaults.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/types.h"

#ifdef HAVE_YAML

void yaml_map_init(yaml_map_t *m)
{
    m->pairs = NULL;
    m->count = 0;
    m->capacity = 0;
}

void yaml_map_add(yaml_map_t *m, const char *key, const char *value)
{
    if (!key || !value)
        return;
    if (m->count >= m->capacity) {
        size_t new_cap = m->capacity == 0 ? 16 : m->capacity * 2;
        yaml_kv_t *new_pairs = (yaml_kv_t *)AIRY_REALLOC(m->pairs, new_cap * sizeof(yaml_kv_t));
        if (!new_pairs)
            return;
        m->pairs = new_pairs;
        m->capacity = new_cap;
    }
    AIRY_STRNCPY_TERM(m->pairs[m->count].key, key, sizeof(m->pairs[m->count].key));
    AIRY_STRNCPY_TERM(m->pairs[m->count].value, value, sizeof(m->pairs[m->count].value));
    m->count++;
}

const char *yaml_map_get(const yaml_map_t *m, const char *key)
{
    if (!m || !key) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    for (size_t i = 0; i < m->count; ++i) {
        if (strcmp(m->pairs[i].key, key) == 0)
            return m->pairs[i].value;
    }
    AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
}

void yaml_map_free(yaml_map_t *m)
{
    AIRY_FREE(m->pairs);
    m->pairs = NULL;
    m->count = 0;
    m->capacity = 0;
}

#endif /* HAVE_YAML */
