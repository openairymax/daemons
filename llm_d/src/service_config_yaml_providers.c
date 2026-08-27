// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_config_yaml_providers.c
 * @brief LLM provider aggregation from model.yaml (split from
 *        service_config.c, 2026-08-27): merge models-section derived
 *        providers with the providers-section overrides and export them as
 *        provider_config_t records.
 *
 * 解析状态来自 service_config_yaml_models.c 的状态机（经
 * llm_service_internal.h 的 HAVE_YAML 节共享）；本文件消费 state 后
 * 通过 svc_yaml_build_result 统一释放或转移所有权。
 */

#include "airy_memory.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm_service_internal.h"

#ifdef HAVE_YAML

/* Aggregate providers from the models list: first model of a provider seeds
 * the provider record, later models append to its model_names. */
static void svc_yaml_aggregate_providers(svc_yaml_state_t *st, provider_agg_t *provs,
                                         size_t *prov_count)
{
    for (size_t i = 0; i < st->model_count; ++i) {
        size_t j = 0;
        for (; j < *prov_count; ++j) {
            if (strcmp(provs[j].name, st->models[i].provider) == 0)
                break;
        }
        if (j == *prov_count) {
            if (*prov_count >= 16)
                break;
            __builtin_memset(&provs[*prov_count], 0, sizeof(provider_agg_t));
            AIRY_STRNCPY_TERM(provs[*prov_count].name, st->models[i].provider,
                              sizeof(provs[*prov_count].name));
            if (st->models[i].api_key_env[0])
                AIRY_STRNCPY_TERM(provs[*prov_count].api_key_env, st->models[i].api_key_env,
                                  sizeof(provs[*prov_count].api_key_env));
            (*prov_count)++;
        }
        if (provs[j].model_count < 64) {
            provs[j].model_names[provs[j].model_count++] = AIRY_STRDUP(st->models[i].name);
        }
        if (!provs[j].base_url[0] && st->models[i].endpoint[0]) {
            const char *suffix = strstr(st->models[i].endpoint, "/chat/completions");
            if (!suffix)
                suffix = strstr(st->models[i].endpoint, "/messages");
            if (suffix && suffix != st->models[i].endpoint) {
                size_t base_len = (size_t)(suffix - st->models[i].endpoint);
                if (base_len < sizeof(provs[j].base_url)) {
                    __builtin_memcpy(provs[j].base_url, st->models[i].endpoint, base_len);
                    provs[j].base_url[base_len] = '\0';
                }
            } else {
                AIRY_STRNCPY_TERM(provs[j].base_url, st->models[i].endpoint,
                                  sizeof(provs[j].base_url));
            }
        }
        if (st->models[i].timeout_sec > provs[j].timeout_sec)
            provs[j].timeout_sec = st->models[i].timeout_sec;
        if (st->models[i].max_retries > provs[j].max_retries)
            provs[j].max_retries = st->models[i].max_retries;
    }
}

/* Merge the providers-section parse results (authoritative base_url/
 * api_key_env source) into the aggregated provider records. */
static void svc_yaml_merge_provider_cfgs(svc_yaml_state_t *st, provider_agg_t *provs,
                                         size_t *prov_count)
{
    for (size_t pi = 0; pi < st->pcfg_count; ++pi) {
        size_t j = 0;
        for (; j < *prov_count; ++j) {
            if (strcmp(provs[j].name, st->pcfg[pi].name) == 0)
                break;
        }
        if (j == *prov_count) {
            if (*prov_count >= 16) {
                for (size_t k = 0; k < st->pcfg[pi].model_count; ++k)
                    AIRY_FREE(st->pcfg[pi].model_names[k]);
                continue;
            }
            __builtin_memset(&provs[*prov_count], 0, sizeof(provider_agg_t));
            AIRY_STRNCPY_TERM(provs[*prov_count].name, st->pcfg[pi].name,
                              sizeof(provs[*prov_count].name));
            (*prov_count)++;
        }

        if (st->pcfg[pi].base_url[0])
            AIRY_STRNCPY_TERM(provs[j].base_url, st->pcfg[pi].base_url,
                              sizeof(provs[j].base_url));
        if (st->pcfg[pi].api_key_env[0])
            AIRY_STRNCPY_TERM(provs[j].api_key_env, st->pcfg[pi].api_key_env,
                              sizeof(provs[j].api_key_env));
        if (st->pcfg[pi].timeout_sec > provs[j].timeout_sec)
            provs[j].timeout_sec = st->pcfg[pi].timeout_sec;
        if (st->pcfg[pi].max_retries > provs[j].max_retries)
            provs[j].max_retries = st->pcfg[pi].max_retries;

        for (size_t k = 0; k < st->pcfg[pi].model_count; ++k) {
            int dup = 0;
            for (size_t m = 0; m < provs[j].model_count; ++m) {
                if (strcmp(provs[j].model_names[m], st->pcfg[pi].model_names[k]) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) {
                AIRY_FREE(st->pcfg[pi].model_names[k]);
            } else if (provs[j].model_count < 64) {
                provs[j].model_names[provs[j].model_count++] = st->pcfg[pi].model_names[k];
            } else {
                AIRY_FREE(st->pcfg[pi].model_names[k]);
            }
        }
    }
}

static int svc_yaml_build_result(provider_agg_t *provs, size_t prov_count,
                                 provider_config_t **out_providers, size_t *out_count)
{
    provider_config_t *result =
        (provider_config_t *)AIRY_CALLOC(prov_count + 1, sizeof(provider_config_t));
    if (!result) {
        for (size_t j = 0; j < prov_count; ++j) {
            for (size_t k = 0; k < provs[j].model_count; ++k)
                AIRY_FREE(provs[j].model_names[k]);
        }
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < prov_count; ++i) {
        result[i].name = AIRY_STRDUP(provs[i].name);
        if (provs[i].api_key_env[0]) {
            char env_prefix[8] = "env:";
            size_t env_key_len = strlen(provs[i].api_key_env);
            char *key_buf = (char *)AIRY_MALLOC(4 + env_key_len + 1);
            if (key_buf) {
                __builtin_memcpy(key_buf, env_prefix, 4);
                __builtin_memcpy(key_buf + 4, provs[i].api_key_env, env_key_len + 1);
                result[i].api_key = key_buf;
            }
        }
        if (provs[i].base_url[0])
            result[i].api_base = AIRY_STRDUP(provs[i].base_url);
        result[i].timeout_sec = (double)provs[i].timeout_sec;
        result[i].max_retries = provs[i].max_retries;
        if (provs[i].model_count > 0) {
            char **marr = (char **)AIRY_CALLOC(provs[i].model_count + 1, sizeof(char *));
            if (marr) {
                for (size_t k = 0; k < provs[i].model_count; ++k)
                    marr[k] = provs[i].model_names[k];
                marr[provs[i].model_count] = NULL;
            } else {
                for (size_t k = 0; k < provs[i].model_count; ++k)
                    AIRY_FREE(provs[i].model_names[k]);
            }
            result[i].models = marr;
        }
    }

    *out_providers = result;
    *out_count = prov_count;
    return AIRY_OK;
}

int svc_load_model_config_yaml(const char *config_path, provider_config_t **out_providers,
                               size_t *out_count)
{
    if (!config_path || !out_providers || !out_count)
        return AIRY_ERR_INVALID_PARAM;

    *out_providers = NULL;
    *out_count = 0;

    FILE *f = fopen(config_path, "rb");
    if (!f) {
        SVC_LOG_WARN("C-L02: SVC: MODEL-CONFIG-WARN cannot open model config, STACK: "
                     "svc_load_model_config_yaml");
        return 0;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        SVC_LOG_WARN(
            "C-L02: SVC: MODEL-CONFIG-WARN YAML parser init, STACK: svc_load_model_config_yaml");
        return 0;
    }
    yaml_parser_set_input_file(&parser, f);

    svc_yaml_state_t st;
    __builtin_memset(&st, 0, sizeof(st));
    yaml_map_init(&st.item_map);
    yaml_map_init(&st.prov_map);
    int done = 0;
    svc_yaml_event_loop(&parser, &st, &done);

    yaml_parser_delete(&parser);
    fclose(f);
    yaml_map_free(&st.item_map);
    yaml_map_free(&st.prov_map);

    /* Simplified llm section expansion: when the top-level llm: mapping
     * exists and model is non-empty, it takes precedence over the full
     * providers/models (see svc_yaml_expand_llm). */
    svc_yaml_expand_llm(&st, config_path);
    if (st.model_count == 0 && st.pcfg_count == 0) {
        SVC_LOG_WARN(
            "C-L02: SVC: MODEL-CONFIG-WARN no models found, STACK: svc_load_model_config_yaml");
        return 0;
    }

    provider_agg_t provs[16];
    size_t prov_count = 0;
    svc_yaml_aggregate_providers(&st, provs, &prov_count);
    svc_yaml_merge_provider_cfgs(&st, provs, &prov_count);
    int rc = svc_yaml_build_result(provs, prov_count, out_providers, out_count);
    if (rc == AIRY_OK) {
        SVC_LOG_INFO(
            "C-L02: SVC: MODEL-CONFIG-OK YAML providers=%zu, STACK: svc_load_model_config_yaml",
            prov_count);
    }
    return rc;
}

#endif /* HAVE_YAML */
