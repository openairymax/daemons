// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_config_yaml.c
 * @brief LLM config YAML parsing infrastructure (split from
 *        service_config.c, 2026-08-27): flat key/value map plus the
 *        global-section loader backing svc_config_load.
 *
 * 2026-08-27 域拆分（service_config.c 剩余 1039 行 → 5 文件）：
 *   - service_config.c                非 YAML 核心（ends_with / JSON 定价 /
 *                                     svc_config_load 分发 / 模型配置分发）
 *   - service_config_yaml.c           YAML 基础设施（本文件）：kv map +
 *                                     global 段加载
 *   - service_config_yaml_models.c    models 状态机 + 简化 llm 段展开
 *   - service_config_yaml_providers.c provider 聚合导出
 *   - service_config_yaml_pricing.c   YAML 定价规则提取
 *
 * kv map 与解析状态结构经 llm_service_internal.h 的 HAVE_YAML 节共享，
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

#include "llm_service_internal.h"

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

int svc_config_load_yaml(const char *config_path, service_config_t *cfg)
{
    if (!cfg || !config_path)
        return AIRY_ERR_INVALID_PARAM;

    __builtin_memset(cfg, 0, sizeof(service_config_t));
    cfg->llm_cache_capacity = AIRY_DEFAULT_CACHE_CAPACITY;
    cfg->llm_cache_ttl_sec = AIRY_DEFAULT_CACHE_TTL_SEC;
    cfg->max_retries = AIRY_DEFAULT_MAX_RETRIES;
    cfg->timeout_ms = AIRY_DEFAULT_TIMEOUT_MS;

    FILE *f = fopen(config_path, "rb");
    if (!f) {
        SVC_LOG_WARN("C-L02: SVC: MODEL-CONFIG-WARN cannot open YAML, STACK: svc_config_load_yaml");
        return 0;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        SVC_LOG_WARN("C-L02: SVC: MODEL-CONFIG-WARN YAML parser init, STACK: svc_config_load_yaml");
        return 0;
    }
    yaml_parser_set_input_file(&parser, f);

    yaml_event_t event;
    yaml_map_t current_map;
    yaml_map_init(&current_map);
    int in_global = 0;
    int done = 0;

    while (!done) {
        if (!yaml_parser_parse(&parser, &event)) {
            SVC_LOG_WARN(
                "C-L02: SVC: MODEL-CONFIG-WARN YAML parse error, STACK: svc_config_load_yaml");
            break;
        }
        if (event.type == YAML_STREAM_END_EVENT) {
            done = 1;
        } else if (event.type == YAML_SCALAR_EVENT) {
            const char *val = (const char *)event.data.scalar.value;
            if (val && strcmp(val, "global") == 0) {
                in_global = 1;
            } else if (in_global && val) {
                char key_buf[128];
                AIRY_STRNCPY_TERM(key_buf, val, sizeof(key_buf));

                yaml_event_t val_event;
                if (yaml_parser_parse(&parser, &val_event)) {
                    if (val_event.type == YAML_SCALAR_EVENT) {
                        const char *v = (const char *)val_event.data.scalar.value;
                        if (v)
                            yaml_map_add(&current_map, key_buf, v);
                    }
                    yaml_event_delete(&val_event);
                }
            }
        } else if (event.type == YAML_MAPPING_END_EVENT) {
            in_global = 0;
        }
        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);
    fclose(f);

    const char *val;
    if ((val = yaml_map_get(&current_map, "llm_cache_capacity"))) {
        cfg->llm_cache_capacity = (size_t)atol(val);
    }
    if ((val = yaml_map_get(&current_map, "llm_cache_ttl_sec"))) {
        cfg->llm_cache_ttl_sec = (uint32_t)atol(val);
    }
    if ((val = yaml_map_get(&current_map, "max_retries"))) {
        cfg->max_retries = (int)strtol(val, NULL, 10);
    }
    if ((val = yaml_map_get(&current_map, "timeout_ms"))) {
        cfg->timeout_ms = (uint32_t)atol(val);
    }
    if ((val = yaml_map_get(&current_map, "token_encoding"))) {
        size_t enc_len = strlen(val);
        if (enc_len < sizeof(cfg->token_encoding)) {
            __builtin_memcpy((char *)cfg->token_encoding, val, enc_len + 1);
        }
    }

    yaml_map_free(&current_map);
    SVC_LOG_INFO("C-L02: SVC: MODEL-CONFIG-OK YAML loaded, STACK: svc_config_load_yaml");
    return AIRY_OK;
}

#endif /* HAVE_YAML */
