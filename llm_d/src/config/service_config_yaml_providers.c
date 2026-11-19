// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_config_yaml_providers.c
 * @brief LLM provider aggregation from model.yaml (split from
 *        service_config.c, 2026-08-27): merge models-section derived
 *        providers with the providers-section overrides and export them as
 *        provider_config_t records.
 *
 * 解析状态来自 service_config_yaml_models.c 对 commons yaml_minimal
 * node 树的 walk（经 config/types.h 共享）；本文件消费 state 后
 * 通过 svc_yaml_build_result 统一释放或转移所有权。
 */

#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/types.h"

/* Find-by-name or append a fresh slot. Returns (size_t)-1 when the fixed
 * 16-slot table is full; the caller owns its overflow policy. */
static size_t agg_slot(provider_agg_t *provs, size_t *prov_count, const char *name,
                       const char *env)
{
    for (size_t j = 0; j < *prov_count; ++j) {
        if (strcmp(provs[j].name, name) == 0)
            return j;
    }
    if (*prov_count >= 16)
        return (size_t)-1;
    provider_agg_t *p = &provs[*prov_count];
    __builtin_memset(p, 0, sizeof(*p));
    AIRY_STRNCPY_TERM(p->name, name, sizeof(p->name));
    if (env && env[0])
        AIRY_STRNCPY_TERM(p->api_key_env, env, sizeof(p->api_key_env));
    return (*prov_count)++;
}

/* 从端点 URL 派生 base_url：剥掉 /chat/completions、/messages 后缀。 */
static void agg_base_url(provider_agg_t *p, const char *endpoint)
{
    if (p->base_url[0] || !endpoint[0])
        return;
    const char *suffix = strstr(endpoint, "/chat/completions");
    if (!suffix)
        suffix = strstr(endpoint, "/messages");
    if (suffix && suffix != endpoint) {
        size_t base_len = (size_t)(suffix - endpoint);
        if (base_len < sizeof(p->base_url)) {
            __builtin_memcpy(p->base_url, endpoint, base_len);
            p->base_url[base_len] = '\0';
        }
    } else {
        AIRY_STRNCPY_TERM(p->base_url, endpoint, sizeof(p->base_url));
    }
}

/* A7-4：能力位取首个显式声明（与 api_format/extra_headers 的聚合惯例
 * 同构）；显式 false 也是声明，整表替换不继承兄弟行。B-1：加速器/显存/
 * 上下文等能力位同样取首个显式声明行，缺省保持未知。 */
static void agg_take_caps(provider_agg_t *p, const model_entry_t *m)
{
    if (!p->has_caps && m->has_caps) {
        p->caps_override = m->caps_override;
        p->has_caps = 1;
    }
    if (!p->hw_caps.declared && m->hw_caps.declared)
        p->hw_caps = m->hw_caps;
}

/* 追加一个模型（上限 64），并补齐 provider 级声明位。 */
static void agg_add_model(provider_agg_t *p, const model_entry_t *m)
{
    if (p->model_count < 64) {
        size_t mi = p->model_count;
        p->model_names[mi] = AIRY_STRDUP(m->name);
        p->model_max_output[mi] = m->max_output_tokens;
        p->model_max_concurrent[mi] = m->max_concurrent;
        p->model_count++;
    }
    agg_base_url(p, m->endpoint);
    if (!p->api_format[0] && m->api_format[0])
        AIRY_STRNCPY_TERM(p->api_format, m->api_format, sizeof(p->api_format));
    if (!p->extra_headers[0] && m->extra_headers[0])
        AIRY_STRNCPY_TERM(p->extra_headers, m->extra_headers, sizeof(p->extra_headers));
    agg_take_caps(p, m);
    if (m->timeout_sec > p->timeout_sec)
        p->timeout_sec = m->timeout_sec;
    if (m->max_retries > p->max_retries)
        p->max_retries = m->max_retries;
}

/* Aggregate providers from the models list: first model of a provider seeds
 * the provider record, later models append to its model_names. */
static void svc_yaml_aggregate_providers(svc_yaml_state_t *st, provider_agg_t *provs,
                                         size_t *prov_count)
{
    for (size_t i = 0; i < st->model_count; ++i) {
        size_t j = agg_slot(provs, prov_count, st->models[i].provider,
                            st->models[i].api_key_env);
        if (j == (size_t)-1)
            break;
        agg_add_model(&provs[j], &st->models[i]);
    }
}

/* Merge the providers-section parse results (authoritative base_url/
 * api_key_env source) into the aggregated provider records. */
static void svc_yaml_merge_provider_cfgs(svc_yaml_state_t *st, provider_agg_t *provs,
                                         size_t *prov_count)
{
    for (size_t pi = 0; pi < st->pcfg_count; ++pi) {
        size_t j = agg_slot(provs, prov_count, st->pcfg[pi].name, NULL);
        if (j == (size_t)-1) {
            for (size_t k = 0; k < st->pcfg[pi].model_count; ++k)
                AIRY_FREE(st->pcfg[pi].model_names[k]);
            continue;
        }

        if (st->pcfg[pi].base_url[0])
            AIRY_STRNCPY_TERM(provs[j].base_url, st->pcfg[pi].base_url,
                              sizeof(provs[j].base_url));
        if (st->pcfg[pi].api_key_env[0])
            AIRY_STRNCPY_TERM(provs[j].api_key_env, st->pcfg[pi].api_key_env,
                              sizeof(provs[j].api_key_env));
        if (st->pcfg[pi].api_format[0])
            AIRY_STRNCPY_TERM(provs[j].api_format, st->pcfg[pi].api_format,
                              sizeof(provs[j].api_format));
        if (st->pcfg[pi].extra_headers[0])
            AIRY_STRNCPY_TERM(provs[j].extra_headers, st->pcfg[pi].extra_headers,
                              sizeof(provs[j].extra_headers));
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
                size_t mi = provs[j].model_count;
                provs[j].model_names[mi] = st->pcfg[pi].model_names[k];
                /* providers: 段只声明连接，不声明每模型输出上限/并发预算；
                 * 保持 0（未配置），由引擎级默认兜底。 */
                provs[j].model_max_output[mi] = 0;
                provs[j].model_max_concurrent[mi] = 0;
                provs[j].model_count++;
            } else {
                AIRY_FREE(st->pcfg[pi].model_names[k]);
            }
        }
    }
}

/* 转移 model_names 所有权到 result；分配失败则释放名字并留空（引擎走默认）。 */
static void build_models(provider_agg_t *p, provider_config_t *out)
{
    if (p->model_count == 0)
        return;
    char **marr = (char **)AIRY_CALLOC(p->model_count + 1, sizeof(char *));
    int *caparr = (int *)AIRY_CALLOC(p->model_count + 1, sizeof(int));
    int *cncarr = (int *)AIRY_CALLOC(p->model_count + 1, sizeof(int));
    if (marr && caparr && cncarr) {
        for (size_t k = 0; k < p->model_count; ++k) {
            marr[k] = p->model_names[k];
            caparr[k] = p->model_max_output[k];
            cncarr[k] = p->model_max_concurrent[k];
        }
        marr[p->model_count] = NULL;
        out->models = marr;
        out->model_max_output = caparr;
        out->model_max_concurrent = cncarr;
    } else {
        AIRY_FREE(marr);
        AIRY_FREE(caparr);
        AIRY_FREE(cncarr);
        for (size_t k = 0; k < p->model_count; ++k)
            AIRY_FREE(p->model_names[k]);
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
            size_t n = 4 + strlen(provs[i].api_key_env) + 1;
            char *key = (char *)AIRY_MALLOC(n);
            if (key) {
                snprintf(key, n, "env:%s", provs[i].api_key_env);
                result[i].api_key = key;
            }
        }
        if (provs[i].base_url[0])
            result[i].api_base = AIRY_STRDUP(provs[i].base_url);
        if (provs[i].api_format[0])
            result[i].api_format = AIRY_STRDUP(provs[i].api_format);
        /* 能力位唯一来源：model.yaml capabilities 声明。未声明即全 false。 */
        result[i].caps = provs[i].caps_override;
        result[i].hw = provs[i].hw_caps;
        if (provs[i].extra_headers[0])
            result[i].extra_headers = AIRY_STRDUP(provs[i].extra_headers);
        result[i].timeout_sec = (double)provs[i].timeout_sec;
        result[i].max_retries = provs[i].max_retries;
        build_models(&provs[i], &result[i]);
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

    svc_yaml_state_t st;
    __builtin_memset(&st, 0, sizeof(st));
    int lrc = svc_yaml_load_state(config_path, &st);
    if (lrc != AIRY_OK) {
        /* 读不到就是失败：返回 0 会把「文件缺失/半写」伪装成「加载成功但
         * 没有 provider」，启动与热重载都据此保持静默（0.1.20 P-A）。 */
        SVC_LOG_ERROR("C-L02: SVC: MODEL-CONFIG-FAIL cannot read %s (rc=%d), STACK: "
                      "svc_load_model_config_yaml",
                      config_path, lrc);
        return lrc;
    }

    /* Simplified llm section expansion: when the top-level llm: mapping
     * exists and model is non-empty, it takes precedence over the full
     * providers/models (see svc_yaml_expand_llm). */
    svc_yaml_expand_llm(&st);
    if (st.model_count == 0 && st.pcfg_count == 0) {
        SVC_LOG_ERROR("C-L02: SVC: MODEL-CONFIG-FAIL %s declares no models and no providers, "
                      "STACK: svc_load_model_config_yaml",
                      config_path);
        return AIRY_ERR_STATE_ERROR;
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
