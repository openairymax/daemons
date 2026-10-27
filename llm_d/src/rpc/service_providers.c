// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_providers.c
 * @brief LLM service provider-management domain: provider-config
 *        copy/merge/release and llm_router endpoint registration.
 */

#include "airy_memory.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include "accounting/cost_tracker.h"
#include "router/core/llm_router.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "rpc/rpc_internal.h"

static void free_provider_fields(provider_config_t *prov)
{
    if (!prov)
        return;
    AIRY_FREE((void *)prov->name);
    AIRY_FREE((void *)prov->api_key);
    AIRY_FREE((void *)prov->api_base);
    AIRY_FREE((void *)prov->organization);
    AIRY_FREE((void *)prov->api_format);
    AIRY_FREE((void *)prov->endpoint_kind);
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
    if (src->api_format)
        d.api_format = AIRY_STRDUP(src->api_format);
    if (src->endpoint_kind)
        d.endpoint_kind = AIRY_STRDUP(src->endpoint_kind);
    d.caps = src->caps;
    if (src->extra_headers)
        d.extra_headers = AIRY_STRDUP(src->extra_headers);
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
        if (src->model_max_output && n > 0) {
            int *carr = (int *)AIRY_CALLOC(n, sizeof(int));
            if (carr) {
                for (size_t i = 0; i < n; ++i)
                    carr[i] = src->model_max_output[i];
                d.model_max_output = carr;
            }
        }
        if (src->model_max_concurrent && n > 0) {
            int *cnc = (int *)AIRY_CALLOC(n, sizeof(int));
            if (cnc) {
                for (size_t i = 0; i < n; ++i)
                    cnc[i] = src->model_max_concurrent[i];
                d.model_max_concurrent = cnc;
            }
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

/* Router-table registration context: prices come from the same rule table
 * the cost tracker consumes, so the model table has a single price source
 * (model.yaml). Latency and capability fields carry protocol-neutral
 * defaults; the latency default is only a seed — llm_rtr_meas() backfills
 * avg_latency_ms with per-request EWMA once real traffic flows. The routing
 * path only ever requires chat/completion/streaming. */
typedef struct {
    const pricing_rule_t *rules;
    int rule_count;
    int registered;
} router_reg_t;

static int register_endpoint_cb(const char *provider_name, const char *model_name, void *user_data)
{
    router_reg_t *reg = (router_reg_t *)user_data;

    llm_endpoint_t ep;
    __builtin_memset(&ep, 0, sizeof(ep));
    snprintf(ep.provider_name, sizeof(ep.provider_name), "%s", provider_name);
    snprintf(ep.model_name, sizeof(ep.model_name), "%s", model_name);

    ep.enabled = true;
    ep.priority = 0;
    ep.context_window = 8192;
    ep.avg_latency_ms = 1000;
    ep.capabilities = LLM_CAP_CHAT | LLM_CAP_COMPLETION | LLM_CAP_STREAMING;
    cost_tracker_price(reg->rules, reg->rule_count, model_name, &ep.cost_per_1k_input,
                       &ep.cost_per_1k_output);

    int rc = llm_router_register_endpoint(&ep);
    if (rc == 0) {
        reg->registered++;
    } else {
        SVC_LOG_WARN("C-L02: SVC: router endpoint register FAILED for %s/%s (rc=%d)", provider_name,
                     model_name, rc);
    }
    return 0;
}

void register_router_endpoints(llm_service_t *svc)
{
    router_reg_t reg = {(const pricing_rule_t *)svc->rules, (int)svc->rule_count, 0};
    provider_registry_enumerate(svc->registry, register_endpoint_cb, &reg);
    SVC_LOG_INFO("C-L02: SVC: registered %d endpoints into llm_router", reg.registered);
}
