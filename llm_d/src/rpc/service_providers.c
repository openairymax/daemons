// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_providers.c
 * @brief LLM service provider-config domain: copy / merge / release.
 */

#include "airy_memory.h"

#include <string.h>

#include "rpc/prov_internal.h"

static void free_provider_fields(provider_config_t *prov)
{
    if (!prov)
        return;
    AIRY_FREE((void *)prov->name);
    AIRY_FREE((void *)prov->api_key);
    AIRY_FREE((void *)prov->api_base);
    AIRY_FREE((void *)prov->organization);
    AIRY_FREE((void *)prov->api_format);
    AIRY_FREE((void *)prov->extra_headers);
    if (prov->models) {
        for (size_t j = 0; prov->models[j]; ++j)
            AIRY_FREE(prov->models[j]);
        AIRY_FREE(prov->models);
    }
    AIRY_FREE((void *)prov->model_max_output);
    AIRY_FREE((void *)prov->model_max_concurrent);
    __builtin_memset(prov, 0, sizeof(*prov));
}

void free_provider_configs(provider_config_t *providers, size_t count)
{
    if (!providers)
        return;
    for (size_t i = 0; i < count; ++i)
        free_provider_fields(&providers[i]);
    AIRY_FREE(providers);
}

static char *dup_opt(const char *s)
{
    return s ? AIRY_STRDUP(s) : NULL;
}

static size_t strv_len(char *const *v)
{
    size_t n = 0;

    while (v[n])
        n++;
    return n;
}

static char **strv_dup(char *const *src, size_t n)
{
    char **dup = (char **)AIRY_CALLOC(n + 1, sizeof(char *));

    if (!dup)
        return NULL;
    for (size_t i = 0; i < n; ++i)
        dup[i] = AIRY_STRDUP(src[i]);
    return dup;
}

static int *ints_dup(const int *src, size_t n)
{
    int *dup = (int *)AIRY_CALLOC(n, sizeof(int));

    if (!dup)
        return NULL;
    for (size_t i = 0; i < n; ++i)
        dup[i] = src[i];
    return dup;
}

/* 模型表与两张 per-model 整数表同长同序，长度都由模型表本身决定；空表
 * 只复制空壳，不分配长度为零的整数表。 */
static void models_take(const provider_config_t *src, provider_config_t *d)
{
    if (!src->models)
        return;

    size_t n = strv_len(src->models);
    d->models = strv_dup(src->models, n);
    if (n == 0)
        return;
    if (src->model_max_output)
        d->model_max_output = ints_dup(src->model_max_output, n);
    if (src->model_max_concurrent)
        d->model_max_concurrent = ints_dup(src->model_max_concurrent, n);
}

static provider_config_t provider_config_dup(const provider_config_t *src)
{
    provider_config_t d;
    __builtin_memset(&d, 0, sizeof(d));
    if (!src)
        return d;

    d.name = dup_opt(src->name);
    d.api_key = dup_opt(src->api_key);
    d.api_base = dup_opt(src->api_base);
    d.organization = dup_opt(src->organization);
    d.api_format = dup_opt(src->api_format);
    d.caps = src->caps;
    d.extra_headers = dup_opt(src->extra_headers);
    d.timeout_sec = src->timeout_sec;
    d.max_retries = src->max_retries;
    models_take(src, &d);
    return d;
}

/* 同名就地替换：只释放该元素的字段，数组本体复用，避免 use-after-free；
 * 用户项胜出。返回 1 表示已替换。 */
static int merged_put(provider_config_t *merged, size_t mc, const provider_config_t *src)
{
    size_t j = 0;

    for (; j < mc; ++j) {
        if (merged[j].name && strcmp(merged[j].name, src->name) == 0)
            break;
    }
    if (j == mc)
        return 0;
    free_provider_fields(&merged[j]);
    merged[j] = provider_config_dup(src);
    return 1;
}

static void merged_add(provider_config_t *merged, size_t cap, size_t *mc,
                       const provider_config_t *src)
{
    if (*mc >= cap)
        return;

    provider_config_t d = provider_config_dup(src);
    if (d.name)
        merged[(*mc)++] = d;
}

void merge_provider_configs(const provider_config_t *main_provs, size_t main_cnt,
                            const provider_config_t *user_provs, size_t user_cnt,
                            provider_config_t **out, size_t *out_cnt)
{
    *out = NULL;
    *out_cnt = 0;
    if ((!main_provs || main_cnt == 0) && (!user_provs || user_cnt == 0))
        return;

    size_t cap = main_cnt + user_cnt + 1;
    provider_config_t *merged = (provider_config_t *)AIRY_CALLOC(cap, sizeof(provider_config_t));
    if (!merged)
        return;

    size_t mc = 0;
    for (size_t i = 0; i < main_cnt && mc < cap; ++i)
        merged_add(merged, cap, &mc, &main_provs[i]);

    for (size_t u = 0; u < user_cnt; ++u) {
        if (!user_provs[u].name)
            continue;
        if (!merged_put(merged, mc, &user_provs[u]))
            merged_add(merged, cap, &mc, &user_provs[u]);
    }
    *out = merged;
    *out_cnt = mc;
}
