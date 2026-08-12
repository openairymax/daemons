// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_providers.c
 * @brief LLM service provider-management domain: provider-config
 *        copy/merge/release, default-model metadata and llm_router
 *        endpoint registration.
 */

#include "airy_memory.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include "providers/registry.h"
#include "router/llm_router.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "llm_service_internal.h"

static void free_provider_fields(provider_config_t *prov)
{
    if (!prov)
        return;
    AIRY_FREE((void *)prov->name);
    AIRY_FREE((void *)prov->api_key);
    AIRY_FREE((void *)prov->api_base);
    AIRY_FREE((void *)prov->organization);
    if (prov->models) {
        for (size_t j = 0; prov->models[j]; ++j)
            AIRY_FREE(prov->models[j]);
        AIRY_FREE(prov->models);
    }
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

static provider_config_t provider_config_dup(const provider_config_t *src)
{
    provider_config_t d;
    __builtin_memset(&d, 0, sizeof(d));
    if (!src)
        return d;
    if (src->name)
        d.name = AIRY_STRDUP(src->name);
    if (src->api_key)
        d.api_key = AIRY_STRDUP(src->api_key);
    if (src->api_base)
        d.api_base = AIRY_STRDUP(src->api_base);
    if (src->organization)
        d.organization = AIRY_STRDUP(src->organization);
    d.timeout_sec = src->timeout_sec;
    d.max_retries = src->max_retries;
    if (src->models) {
        size_t n = 0;
        while (src->models[n])
            n++;
        char **marr = (char **)AIRY_CALLOC(n + 1, sizeof(char *));
        if (marr) {
            for (size_t i = 0; i < n; ++i)
                marr[i] = AIRY_STRDUP(src->models[i]);
            d.models = marr;
        }
    }
    return d;
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
    for (size_t i = 0; i < main_cnt && mc < cap; ++i) {
        provider_config_t d = provider_config_dup(&main_provs[i]);
        if (d.name)
            merged[mc++] = d;
    }
    for (size_t u = 0; u < user_cnt; ++u) {
        if (!user_provs[u].name)
            continue;
        size_t j = 0;
        for (; j < mc; ++j) {
            if (merged[j].name && strcmp(merged[j].name, user_provs[u].name) == 0)
                break;
        }
        if (j < mc) {
            /* Same-name replacement: only free the old fields of this element
             * (never the array itself, avoiding use-after-free), then copy the
             * user item (user wins) */
            free_provider_fields(&merged[j]);
            merged[j] = provider_config_dup(&user_provs[u]);
        } else if (mc < cap) {
            provider_config_t d = provider_config_dup(&user_provs[u]);
            if (d.name)
                merged[mc++] = d;
        }
    }
    *out = merged;
    *out_cnt = mc;
}

/* Default model metadata — kept consistent with the default pricing rules of
 * llm_router_init's cost_tracker. Used to fill the cost/latency/caps fields at
 * endpoint registration; future versions may override via config file. */
typedef struct {
    const char *prefix;
    double cost_per_1k_input;
    double cost_per_1k_output;
    uint32_t avg_latency_ms;
    uint32_t capabilities;
} model_default_meta_t;

static const model_default_meta_t MODEL_DEFAULT_META[] = {
    {"gpt-4", 0.03, 0.06, 1200,
     LLM_CAP_CHAT | LLM_CAP_COMPLETION | LLM_CAP_STREAMING | LLM_CAP_FUNCTION_CALL},
    {"gpt-3.5", 0.001, 0.002, 1000, LLM_CAP_CHAT | LLM_CAP_COMPLETION | LLM_CAP_STREAMING},
    {"claude", 0.015, 0.075, 1100,
     LLM_CAP_CHAT | LLM_CAP_COMPLETION | LLM_CAP_STREAMING | LLM_CAP_FUNCTION_CALL},
    {"deepseek", 0.00014, 0.00028, 900,
     LLM_CAP_CHAT | LLM_CAP_COMPLETION | LLM_CAP_STREAMING | LLM_CAP_FUNCTION_CALL},
    {"gemini", 0.0005, 0.0015, 1000,
     LLM_CAP_CHAT | LLM_CAP_COMPLETION | LLM_CAP_STREAMING | LLM_CAP_VISION},
};

static const model_default_meta_t *lookup_model_meta(const char *model_name)
{
    if (!model_name)
        return NULL;
    for (size_t i = 0; i < sizeof(MODEL_DEFAULT_META) / sizeof(MODEL_DEFAULT_META[0]); i++) {
        size_t plen = strlen(MODEL_DEFAULT_META[i].prefix);
        if (strncmp(model_name, MODEL_DEFAULT_META[i].prefix, plen) == 0) {
            return &MODEL_DEFAULT_META[i];
        }
    }
    return NULL;
}

static int register_endpoint_cb(const char *provider_name, const char *model_name, void *user_data)
{
    int *registered_count = (int *)user_data;

    llm_endpoint_t ep;
    __builtin_memset(&ep, 0, sizeof(ep));
    snprintf(ep.provider_name, sizeof(ep.provider_name), "%s", provider_name);
    snprintf(ep.model_name, sizeof(ep.model_name), "%s", model_name);

    ep.enabled = true;
    ep.priority = 0;
    ep.context_window = 8192;

    const model_default_meta_t *meta = lookup_model_meta(model_name);
    if (meta) {
        ep.cost_per_1k_input = meta->cost_per_1k_input;
        ep.cost_per_1k_output = meta->cost_per_1k_output;
        ep.avg_latency_ms = meta->avg_latency_ms;
        ep.capabilities = meta->capabilities;
    } else {

        ep.cost_per_1k_input = 0.001;
        ep.cost_per_1k_output = 0.002;
        ep.avg_latency_ms = 1000;
        ep.capabilities = LLM_CAP_CHAT | LLM_CAP_COMPLETION | LLM_CAP_STREAMING;
    }

    int rc = llm_router_register_endpoint(&ep);
    if (rc == 0) {
        (*registered_count)++;
    } else {
        SVC_LOG_WARN("C-L02: SVC: router endpoint register FAILED for %s/%s (rc=%d)", provider_name,
                     model_name, rc);
    }
    return 0;
}

void register_router_endpoints(llm_service_t *svc)
{
    int registered_count = 0;
    provider_registry_enumerate(svc->registry, register_endpoint_cb, &registered_count);
    SVC_LOG_INFO("C-L02: SVC: registered %d endpoints into llm_router", registered_count);
}
