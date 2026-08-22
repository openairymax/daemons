// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_request.c
 * @brief LLM service request-handling domain: cache-key generation,
 *        provider routing selection, full/streaming completion handling
 *        and cost tracking.
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "error.h"
#include "response.h"
#include "router/llm_router.h"
#include "service.h"
#include "svc_logger.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm_service_internal.h"

/**
 * @brief Generate a cache key
 * @param manager Request config
 * @return Cache key string (caller frees), NULL on failure
 */
static char *make_cache_key(const llm_request_config_t *manager)
{
    if (!manager || !manager->model) {
        SVC_LOG_ERROR("make_cache_key: NULL parameter (manager=%p, model=%p)",
                      (const void *)manager, manager ? (const void *)manager->model : NULL);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    size_t len = strlen(manager->model) + 2;
    for (size_t i = 0; i < manager->message_count; ++i) {
        const char *role = manager->messages[i].role ? manager->messages[i].role : "";
        const char *content = manager->messages[i].content ? manager->messages[i].content : "";
        const char *reasoning =
            manager->messages[i].reasoning_content ? manager->messages[i].reasoning_content : "";
        len += strlen(role) + 1 + strlen(content) + 1 + strlen(reasoning) + 1;
    }

    char *key = (char *)AIRY_MALLOC(len);
    if (!key) {
        SVC_LOG_ERROR("make_cache_key: malloc failed for cache key (len=%zu)", len);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    char *p = key;

    size_t pos = 0;
    size_t written = (size_t)snprintf(p, len, "%s", manager->model);
    if (written < len) {
        pos = written;
    } else {
        pos = len > 0 ? len - 1 : 0;
    }
    p[pos] = '|';
    pos++;

    for (size_t i = 0; i < manager->message_count; ++i) {
        const char *role = manager->messages[i].role ? manager->messages[i].role : "";
        const char *content = manager->messages[i].content ? manager->messages[i].content : "";
        const char *reasoning =
            manager->messages[i].reasoning_content ? manager->messages[i].reasoning_content : "";
        size_t remaining = (pos < len) ? (len - pos) : 0;
        written = (size_t)snprintf(p + pos, remaining, "%s:%s:%s|", role, content, reasoning);
        if (written < remaining) {
            pos += written;
        } else {
            pos = len > 0 ? len - 1 : 0;
            break;
        }
    }

    if (pos > 0 && key[pos - 1] == '|') {
        key[pos - 1] = '\0';
    } else if (pos < len) {
        key[pos] = '\0';
    } else {
        key[len - 1] = '\0';
    }

    return key;
}

/**
 * @brief Get the response from the cache
 */
static int get_cached_response(llm_service_t *svc, const char *cache_key,
                               llm_response_t **out_response)
{
    if (!svc || !cache_key || !out_response) {
        SVC_LOG_ERROR("get_cached_response: NULL parameter (svc=%p, cache_key=%p, out_response=%p)",
                      (const void *)svc, (const void *)cache_key, (const void *)out_response);
        return AIRY_ERR_INVALID_PARAM;
    }

    char *cached_json = NULL;
    if (llm_cache_get(svc->cache, cache_key, &cached_json) == 1 && cached_json) {
        llm_response_t *cached_resp = response_from_json(cached_json);
        AIRY_FREE(cached_json);
        cached_json = NULL;

        if (cached_resp) {
            *out_response = cached_resp;
            SVC_LOG_DEBUG("Cache hit for key");
            return 1;
        }
        SVC_LOG_WARN("Failed to parse cached response, fetching fresh data");
    }

    return 0;
}

/**
 * @brief Find a provider
 */
static const provider_t *find_provider(llm_service_t *svc, const char *model)
{
    if (!svc || !model) {
        SVC_LOG_ERROR("find_provider: NULL parameter (svc=%p, model=%p)", (const void *)svc,
                      (const void *)model);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_mtx_lock(&svc->lock);
    const provider_t *prov = provider_registry_find(svc->registry, model);
    airy_mtx_unlock(&svc->lock);

    return prov;
}

/* ---------- P3.16 (ACC-DT17): select provider via llm_router ----------
 *
 * Explicit model wins (GRAD three-model separation):
 *   When the caller explicitly specifies a model (e.g. t2=glm-4,
 *   t1-f=deepseek-chat) and it matches exactly in the registry, return that
 *   provider directly — COST_AWARE routing would pick the "cheapest endpoint"
 *   and may ignore the user-specified model (the risk of t2 being routed to
 *   deepseek). Only when the exact match fails, fall back to strategy routing.
 *
 * On routing failure (e.g. not initialized, no endpoint matching the
 * capabilities, empty registry) return NULL, and the caller falls back to
 * find_provider(model) for backward compatibility. */
static const provider_t *select_provider_via_router(llm_service_t *svc,
                                                    const llm_request_config_t *manager,
                                                    bool is_stream)
{
    if (!svc || !manager) {
        return NULL;
    }

    if (manager->model && manager->model[0]) {
        const provider_t *exact = find_provider(svc, manager->model);
        if (exact) {
            SVC_LOG_INFO("C-L02: SVC: explicit model %s resolved directly "
                         "(provider=%s, skip router)",
                         manager->model, exact->name ? exact->name : "?");
            return exact;
        }
        SVC_LOG_DEBUG("C-L02: SVC: explicit model %s not in registry, "
                      "falling back to router",
                      manager->model);
    }

    llm_route_request_t req;
    __builtin_memset(&req, 0, sizeof(req));

    if (manager->message_count > 0 && manager->messages && manager->messages[0].content) {
        req.prompt = manager->messages[0].content;
        req.prompt_len = strlen(req.prompt);
    } else {
        req.prompt = "";
        req.prompt_len = 0;
    }

    req.required_caps = LLM_CAP_CHAT | LLM_CAP_COMPLETION;
    if (is_stream) {
        req.required_caps |= LLM_CAP_STREAMING;
    }

    req.max_tokens = (manager->max_tokens > 0) ? (uint32_t)manager->max_tokens : 0;
    req.max_cost = 0;
    req.max_latency_ms = 0;
    req.strategy = LLM_ROUTE_COST;
    req.preferred_provider[0] = '\0';

    llm_route_result_t result;
    __builtin_memset(&result, 0, sizeof(result));
    int rc = llm_router_route(&req, &result);
    if (rc != 0) {
        SVC_LOG_DEBUG("C-L02: SVC: router_route rc=%d — will fall back to find_provider(%s)", rc,
                      manager->model ? manager->model : "NULL");
        return NULL;
    }

    /* Resolve the routed model_name to the real provider via the registry.
     * If the routed model does not exist in the registry (theoretically
     * impossible since endpoints originate from the registry), return NULL to
     * trigger the caller's fallback. */
    const provider_t *prov = find_provider(svc, result.model_name);
    if (prov) {
        SVC_LOG_INFO("C-L02: SVC: ROUTED provider=%s model=%s strategy=%d cost=%.6f latency=%u",
                     result.provider_name, result.model_name, (int)result.strategy_used,
                     result.estimated_cost, result.estimated_latency_ms);
    }
    return prov;
}

/**
 * @brief Store the response to the cache
 */
static void cache_response(llm_service_t *svc, const char *cache_key, llm_response_t *resp)
{
    if (!svc || !cache_key || !resp) {
        return;
    }

    char *resp_json = response_to_json(resp);
    if (resp_json) {
        llm_cache_put(svc->cache, cache_key, resp_json);
        AIRY_FREE(resp_json);
        resp_json = NULL;
    }
}

/**
 * @brief Update cost tracking (accumulate + fill per-call cost back into the
 *        response)
 */
static void update_cost_tracking(llm_service_t *svc, const char *model, llm_response_t *resp)
{
    if (!svc || !model || !resp) {
        return;
    }

    cost_tracker_add(svc->cost, model, resp->prompt_tokens, resp->completion_tokens);
    resp->cost_usd =
        cost_tracker_estimate(svc->cost, model, resp->prompt_tokens, resp->completion_tokens);
}

int llm_service_complete(llm_service_t *svc, const llm_request_config_t *manager,
                         llm_response_t **out_response)
{

    if (!svc || !manager || !out_response) {
        SVC_LOG_ERROR("C-L02: SVC: COMPLETE-FAIL invalid arguments, STACK: llm_service_complete");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!manager->model) {
        SVC_LOG_ERROR("C-L02: SVC: COMPLETE-FAIL model=NULL, STACK: llm_service_complete");
        return AIRY_ERR_INVALID_PARAM;
    }

    char *cache_key = make_cache_key(manager);
    if (!cache_key) {
        SVC_LOG_ERROR("C-L02: SVC: COMPLETE-FAIL cache_key alloc, STACK: llm_service_complete");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    llm_response_t *cached_resp = NULL;
    int cache_status = get_cached_response(svc, cache_key, &cached_resp);
    if (cache_status > 0 && cached_resp) {
        AIRY_FREE(cache_key);
        *out_response = cached_resp;
        return AIRY_OK;
    }

    /* P3.16 (ACC-DT17): prefer strategy routing via llm_router to select the
     * provider; on routing failure (not initialized / no capability-matching
     * endpoint / empty registry) fall back to find_provider(manager->model)
     * for backward compatibility. */
    const provider_t *prov = select_provider_via_router(svc, manager, false);
    if (!prov) {
        SVC_LOG_INFO("C-L02: SVC: router miss — falling back to find_provider(model=%s)",
                     manager->model);
        prov = find_provider(svc, manager->model);
    }
    if (!prov) {
        SVC_LOG_ERROR(
            "C-L02: SVC: COMPLETE-FAIL model=%s, error=INVALID_MODEL, STACK: llm_service_complete",
            manager->model);
        AIRY_FREE(cache_key);
        cache_key = NULL;
        return AIRY_ERR_LLM_INVALID_MODEL;
    }

    {

        const char *first_content = NULL;
        size_t input_len = 0;
        if (manager->message_count > 0 && manager->messages[0].content) {
            first_content = manager->messages[0].content;
            input_len = strlen(first_content);
        }
        llm_complexity_level_t complexity = assess_complexity(first_content);
        log_routing_decision(manager->model, complexity, input_len, "user_specified");
    }

    llm_response_t *resp = NULL;
    int ret = prov->ops->complete(prov->ctx, manager, &resp);
    if (ret != 0) {
        SVC_LOG_ERROR("C-L02: SVC: COMPLETE-FAIL model=%s, error=%d, STACK: llm_service_complete",
                      manager->model, ret);
        AIRY_FREE(cache_key);
        cache_key = NULL;
        return ret;
    }

    update_cost_tracking(svc, manager->model, resp);
    cache_response(svc, cache_key, resp);

    *out_response = resp;
    AIRY_FREE(cache_key);
    cache_key = NULL;
    return AIRY_OK;
}

int llm_service_complete_stream(llm_service_t *svc, const llm_request_config_t *manager,
                                llm_stream_callback_t callback, void *callback_data,
                                llm_response_t **out_response)
{

    if (!svc || !manager || !callback) {
        SVC_LOG_ERROR(
            "C-L02: SVC: STREAM-FAIL invalid arguments, STACK: llm_service_complete_stream");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!manager->model) {
        SVC_LOG_ERROR("C-L02: SVC: STREAM-FAIL model=NULL, STACK: llm_service_complete_stream");
        return AIRY_ERR_INVALID_PARAM;
    }

    /* P3.16 (ACC-DT17): prefer strategy routing via llm_router to select the
     * provider; on routing failure fall back to find_provider(manager->model)
     * for backward compatibility. */
    const provider_t *prov = select_provider_via_router(svc, manager, true);
    if (!prov) {
        SVC_LOG_INFO("C-L02: SVC: router miss (stream) — falling back to find_provider(model=%s)",
                     manager->model);
        prov = find_provider(svc, manager->model);
    }
    if (!prov) {
        SVC_LOG_ERROR("C-L02: SVC: STREAM-FAIL model=%s, error=INVALID_MODEL, STACK: "
                      "llm_service_complete_stream",
                      manager->model);
        return AIRY_ERR_LLM_INVALID_MODEL;
    }

    {
        const char *first_content = NULL;
        size_t input_len = 0;
        if (manager->message_count > 0 && manager->messages[0].content) {
            first_content = manager->messages[0].content;
            input_len = strlen(first_content);
        }
        llm_complexity_level_t complexity = assess_complexity(first_content);
        log_routing_decision(manager->model, complexity, input_len, "stream_user_specified");
    }

    if (!prov->ops->complete_stream) {
        SVC_LOG_ERROR("C-L02: SVC: STREAM-FAIL model=%s, error=NOT_SUPPORTED, STACK: "
                      "llm_service_complete_stream",
                      manager->model);
        return AIRY_ERR_NOT_SUPPORTED;
    }

    int ret = prov->ops->complete_stream(prov->ctx, manager, callback, callback_data, out_response);

    if (ret == 0 && out_response && *out_response) {
        llm_response_t *resp = *out_response;
        cost_tracker_add(svc->cost, manager->model, resp->prompt_tokens, resp->completion_tokens);
        resp->cost_usd = cost_tracker_estimate(svc->cost, manager->model, resp->prompt_tokens,
                                               resp->completion_tokens);
    }

    return ret;
}

int llm_service_embeddings(llm_service_t *svc, const char *model, const char *request_body,
                           char **out_json)
{
    if (!svc || !request_body || !request_body[0] || !out_json)
        return AIRY_ERR_INVALID_PARAM;
    *out_json = NULL;

    const char *m = (model && model[0]) ? model : svc->default_model;
    const provider_t *prov = find_provider(svc, m ? m : "");
    if (!prov || !prov->ctx) {
        SVC_LOG_ERROR("embeddings: no provider for model=%s", m ? m : "(default)");
        return AIRY_ERR_LLM_INVALID_MODEL;
    }

    provider_base_ctx_t *base = provider_base_ctx(prov->ctx);
    provider_refresh_api_key(base);

    /* OpenAI 兼容 embeddings 端点：$api_base/embeddings（去掉尾部斜杠防双斜杠） */
    char url[1024];
    size_t blen = strlen(base->api_base);
    while (blen > 0 && (base->api_base[blen - 1] == '/' || base->api_base[blen - 1] == '\\'))
        blen--;
    if (blen == 0) {
        SVC_LOG_ERROR("embeddings: empty api_base for provider=%s", prov->name ? prov->name : "?");
        return AIRY_ERR_INVALID_PARAM;
    }
    snprintf(url, sizeof(url), "%.*s/embeddings", (int)blen, base->api_base);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (base->api_key[0]) {
        char auth[320];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", base->api_key);
        headers = curl_slist_append(headers, auth);
    }

    provider_http_resp_t *resp = NULL;
    long http_code = 0;
    int rc = provider_http_post(url, headers, request_body, base->timeout_sec, base->max_retries,
                                &resp, &http_code);
    if (headers)
        curl_slist_free_all(headers);

    if (rc != 0 || !resp) {
        provider_http_resp_free(resp);
        SVC_LOG_ERROR("embeddings: HTTP request failed url=%s rc=%d", url, rc);
        return AIRY_ERR_IO;
    }

    if (http_code >= 400) {
        SVC_LOG_WARN("embeddings: upstream HTTP %ld url=%s", http_code, url);
        AIRY_FREE(resp->data);
        AIRY_FREE(resp);
        return AIRY_ERR_IO;
    }

    *out_json = AIRY_STRDUP(resp->data ? resp->data : "{}");
    provider_http_resp_free(resp);
    if (!*out_json)
        return AIRY_ERR_OUT_OF_MEMORY;
    return AIRY_SUCCESS;
}
