// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file llm_router.c
 * @brief LLM router orchestrator - unified select + endpoint mgmt + stats.
 *
 * Orchestrates endpoint registration, statistics collection, the default
 * strategy and the four selection rules behind llm_router_route(), all of
 * which live in router/strategy/router_strategy.c.
 */

#include "router/core/router_context.h"
#include "router/core/router_state.h"
#include "airy_memory.h"

static router_ctx_t g_router;

router_ctx_t *router_ctx_get(void)
{
    return &g_router;
}

int llm_router_init(void)
{
    router_ctx_t *ctx = router_ctx_get();

    if (ctx->initialized) {
        AIRY_LOG_INFO("C-L02: LLMRouter: already initialized, skipping");
        return 0;
    }

    AIRY_MEMSET(ctx, 0, sizeof(router_ctx_t));
    ctx->default_strategy = LLM_ROUTE_COST;

    AIRY_LOG_INFO("C-L02: LLMRouter: initializing with default_strategy=COST");

    if (AIRY_MUTEX_INIT(&ctx->mutex, NULL) != 0) {
        AIRY_LOG_ERROR("C-L02: LLMRouter: failed to initialize mutex STACK: llm_router_init");
        return AIRY_ERR_SYS_MUTEX;
    }

    ctx->token_cfg = (airy_token_config_t){.model_type = AIRY_TOKEN_MODEL_BPE,
                                           .model_name = "cl100k_base",
                                           .cjk_ratio = 0.2f,
                                           .alpha_ratio = 0.4f,
                                           .flags = AIRY_TOKEN_FLAG_ACCURATE};

    ctx->initialized = true;
    /* 已落盘的健康面在此读入快照，由后续注册逐个消费；读失败不阻断路由。 */
    router_state_load();

    AIRY_LOG_INFO("C-L02: LLMRouter: initialization complete");
    return 0;
}

void llm_router_destroy(void)
{
    router_ctx_t *ctx = router_ctx_get();

    if (!ctx->initialized) {
        AIRY_LOG_DEBUG("C-L02: LLMRouter: not initialized, skip destroy");
        return;
    }

    AIRY_LOG_INFO("C-L02: LLMRouter: destroying (total_requests=%llu, "
                  "total_cost=$%.6f, total_tokens=%llu, fallbacks=%llu, errors=%llu)",
                  (unsigned long long)ctx->stats.total_requests, ctx->stats.total_cost,
                  (unsigned long long)ctx->stats.total_tokens,
                  (unsigned long long)ctx->stats.fallback_count,
                  (unsigned long long)ctx->stats.error_count);

    AIRY_MUTEX_LOCK(&ctx->mutex);
    (void)router_state_save_locked();
    AIRY_MUTEX_UNLOCK(&ctx->mutex);

    AIRY_MUTEX_DESTROY(&ctx->mutex);
    AIRY_MEMSET(ctx, 0, sizeof(router_ctx_t));

    AIRY_LOG_INFO("C-L02: LLMRouter: destroyed");
}

int llm_router_register_endpoint(const llm_endpoint_t *endpoint)
{
    router_ctx_t *ctx = router_ctx_get();

    if (!endpoint) {
        AIRY_LOG_ERROR("C-L02: LLMRouter: register_endpoint called with NULL endpoint STACK: "
                       "llm_router_register_endpoint");
        return AIRY_ERR_INVALID_PARAM;
    }

    AIRY_MUTEX_LOCK(&ctx->mutex);

    if (ctx->endpoint_count >= LLM_ROUTER_MAX_ENDPOINTS) {
        AIRY_LOG_ERROR("C-L02: LLMRouter: endpoint limit reached (%zu/%d), "
                       "cannot register %s/%s STACK: llm_router_register_endpoint",
                       ctx->endpoint_count, LLM_ROUTER_MAX_ENDPOINTS, endpoint->provider_name,
                       endpoint->model_name);
        AIRY_MUTEX_UNLOCK(&ctx->mutex);
        return AIRY_ERR_OVERFLOW;
    }

    llm_endpoint_t *slot = &ctx->endpoints[ctx->endpoint_count];
    AIRY_MEMCPY(slot, endpoint, sizeof(llm_endpoint_t));
    /* 重注册即重置健康面：配置变更后的端点须从干净状态重新计数，
     * 再按 provider/model 回填落盘状态（重启恢复与热重载复用同一路径）。 */
    slot->consec_fail = 0U;
    slot->last_fail_ms = 0U;
    slot->open_until_ms = 0U;
    slot->probe_until_ms = 0U;
    slot->probe_fail = 0U;
    slot->open_level = 0U;
    router_ep_restore_locked(slot);
    ctx->endpoint_count++;

    AIRY_LOG_INFO("C-L02: LLMRouter: registered endpoint %s/%s (total=%zu, "
                  "caps=0x%x, cost=$%.6f/$%.6f, latency=%ums)",
                  endpoint->provider_name, endpoint->model_name, ctx->endpoint_count,
                  endpoint->capabilities, endpoint->cost_per_1k_input, endpoint->cost_per_1k_output,
                  endpoint->avg_latency_ms);

    AIRY_MUTEX_UNLOCK(&ctx->mutex);
    return 0;
}

int llm_router_reset_endpoints(void)
{
    router_ctx_t *ctx = router_ctx_get();

    if (!ctx->initialized)
        return AIRY_ERR_SYS_NOT_INIT;

    AIRY_MUTEX_LOCK(&ctx->mutex);
    /* 热重载会清表重建：先把当前健康面搬进快照，注册时再逐个回填，
     * 使配置改动不白送一轮熔断状态。 */
    router_state_capture_locked();
    ctx->endpoint_count = 0;
    ctx->round_robin_index = 0;
    AIRY_MUTEX_UNLOCK(&ctx->mutex);

    AIRY_LOG_INFO("C-L02: LLMRouter: endpoint table reset for reload");
    return 0;
}

int llm_router_route(const llm_route_request_t *request, llm_route_result_t *result)
{
    router_ctx_t *ctx = router_ctx_get();

    if (!request || !result) {
        AIRY_LOG_ERROR(
            "C-L02: LLMRouter: route called with NULL request or result STACK: llm_router_route");
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!ctx->initialized) {
        AIRY_LOG_ERROR(
            "C-L02: LLMRouter: route called before initialization STACK: llm_router_route");
        return AIRY_ERR_SYS_NOT_INIT;
    }

    AIRY_MEMSET(result, 0, sizeof(llm_route_result_t));

    llm_route_strategy_t strategy = request->strategy;
    if (strategy >= LLM_ROUTE_COUNT) {
        strategy = ctx->default_strategy;
        AIRY_LOG_DEBUG("C-L02: LLMRouter: invalid strategy, using default=%d", strategy);
    }

    AIRY_LOG_DEBUG("C-L02: LLMRouter: routing request (strategy=%d, caps=0x%x, "
                   "max_cost=$%.6f, max_latency=%ums, prompt_len=%zu)",
                   strategy, request->required_caps, request->max_cost, request->max_latency_ms,
                   request->prompt_len);

    int ret = -1;
    const char *strategy_name = "UNKNOWN";

    switch (strategy) {
    case LLM_ROUTE_COST:
        strategy_name = "COST_AWARE";
        ret = route_cost_aware(request, result);

        if (ret != 0) {
            AIRY_LOG_WARN("C-L02: LLMRouter: cost_aware failed, falling back to round_robin");
            ctx->stats.fallback_count++;
            AIRY_MEMSET(result, 0, sizeof(llm_route_result_t));
            ret = route_round_robin(request, result);
            if (ret == 0) {
                strategy_name = "COST_AWARE->ROUND_ROBIN";
            }
        }
        break;
    case LLM_ROUTE_ROUND_ROBIN:
        strategy_name = "ROUND_ROBIN";
        ret = route_round_robin(request, result);
        break;
    case LLM_ROUTE_LATENCY:
        strategy_name = "LEAST_LATENCY";
        ret = route_least_latency(request, result);
        break;
    case LLM_ROUTE_COMPLEXITY:
        strategy_name = "QUALITY_FIRST";
        ret = route_quality_first(request, result);
        break;
    case LLM_ROUTE_FALLBACK:
        strategy_name = "FALLBACK";

        ret = route_round_robin(request, result);
        if (ret != 0) {
            AIRY_LOG_DEBUG("C-L02: LLMRouter: fallback round_robin failed, "
                           "trying cost_aware");
            ret = route_cost_aware(request, result);
        }
        if (ret != 0) {
            AIRY_LOG_DEBUG("C-L02: LLMRouter: fallback cost_aware failed, "
                           "trying least_latency");
            ret = route_least_latency(request, result);
        }
        break;
    default:
        strategy_name = "DEFAULT->COST_AWARE";
        ret = route_cost_aware(request, result);
        break;
    }

    AIRY_MUTEX_LOCK(&ctx->mutex);
    ctx->stats.total_requests++;
    if (ret == 0) {
        ctx->stats.routed_count[strategy]++;
        ctx->stats.total_cost += result->estimated_cost;

        if (request->prompt) {
            ctx->stats.total_tokens += router_estimate_tokens(request->prompt, request->prompt_len);
        }
        AIRY_LOG_INFO("C-L02: LLMRouter: routed via %s -> %s/%s "
                      "(cost=$%.6f, latency=%ums, confidence=%d%%)",
                      strategy_name, result->provider_name, result->model_name,
                      result->estimated_cost, result->estimated_latency_ms, result->confidence);
    } else {
        ctx->stats.error_count++;
        AIRY_LOG_ERROR("C-L02: LLMRouter: all routing strategies failed for request "
                       "(strategy=%d, caps=0x%x, max_cost=$%.6f, "
                       "max_latency=%ums, prompt_len=%zu, "
                       "total_endpoints=%zu) STACK: llm_router_route",
                       strategy, request->required_caps, request->max_cost, request->max_latency_ms,
                       request->prompt_len, ctx->endpoint_count);
    }
    AIRY_MUTEX_UNLOCK(&ctx->mutex);

    return ret;
}

/* 候选排序判据：priority 降序、估算成本升序；相等时保持注册序（稳定插入排序）。 */
static int cand_precedes(const llm_endpoint_t *a, const llm_endpoint_t *b, size_t in_tok,
                         size_t out_tok)
{
    if (a->priority != b->priority)
        return a->priority > b->priority;
    return router_estimate_cost(a, in_tok, out_tok) < router_estimate_cost(b, in_tok, out_tok);
}

int llm_router_pick(const llm_route_request_t *request, llm_route_result_t *out, size_t max,
                          size_t *out_count)
{
    router_ctx_t *ctx = router_ctx_get();
    llm_endpoint_t *eps[LLM_ROUTER_MAX_ENDPOINTS];
    size_t n;
    size_t in_tok;
    size_t out_tok;
    size_t take;

    if (!request || !out || max == 0 || !out_count)
        return AIRY_ERR_INVALID_PARAM;

    *out_count = 0;
    if (!ctx->initialized)
        return AIRY_ERR_SYS_NOT_INIT;

    n = router_get_eligible(request, eps, LLM_ROUTER_MAX_ENDPOINTS);
    in_tok = router_estimate_tokens(request->prompt, request->prompt_len);
    out_tok = request->max_tokens ? request->max_tokens : LLM_ROUTE_EST_OUTPUT_TOKENS;

    for (size_t i = 1; i < n; i++) {
        llm_endpoint_t *key = eps[i];
        size_t j = i;

        while (j > 0 && cand_precedes(key, eps[j - 1], in_tok, out_tok)) {
            eps[j] = eps[j - 1];
            j--;
        }
        eps[j] = key;
    }

    take = n < max ? n : max;
    for (size_t i = 0; i < take; i++)
        router_fill_result(&out[i], eps[i], LLM_ROUTE_COST, 100, in_tok, out_tok);
    *out_count = take;
    return 0;
}

int llm_rtr_meas(const char *provider, const char *model, const llm_response_t *resp,
                 uint64_t lat_ms)
{
    router_ctx_t *ctx = router_ctx_get();
    int fresh = provider && model;

    if (!resp)
        return AIRY_ERR_INVALID_PARAM;
    if (!ctx->initialized)
        return AIRY_ERR_SYS_NOT_INIT;

    AIRY_MUTEX_LOCK(&ctx->mutex);

    if (fresh) {
        llm_endpoint_t *ep = router_ep_find(provider, model);
        if (ep)
            ep->avg_latency_ms =
                (uint32_t)(((uint64_t)ep->avg_latency_ms * 4ULL + lat_ms + 2ULL) / 5ULL);
        ctx->stats.lat_samples++;
        ctx->stats.lat_total_ms += lat_ms;
    }

    ctx->stats.prompt_tokens_sum += resp->prompt_tokens;
    ctx->stats.cached_tokens_sum += resp->cached_tokens;

    if (fresh && (resp->timings.prefill_tokens || resp->timings.decode_tokens ||
                  resp->timings.kv_cache_tokens || resp->timings.prefill_ms != 0.0 ||
                  resp->timings.decode_ms != 0.0)) {
        ctx->stats.meas_samples++;
        ctx->stats.prefill_tokens += resp->timings.prefill_tokens;
        ctx->stats.decode_tokens += resp->timings.decode_tokens;
        ctx->stats.kv_cache_tokens += resp->timings.kv_cache_tokens;
        ctx->stats.prefill_ms += resp->timings.prefill_ms;
        ctx->stats.decode_ms += resp->timings.decode_ms;
    }

    AIRY_MUTEX_UNLOCK(&ctx->mutex);
    return 0;
}

int llm_router_get_stats(llm_router_stats_t *stats)
{
    router_ctx_t *ctx = router_ctx_get();

    if (!stats) {
        AIRY_LOG_ERROR(
            "C-L02: LLMRouter: get_stats called with NULL stats STACK: llm_router_get_stats");
        return AIRY_ERR_INVALID_PARAM;
    }

    AIRY_MUTEX_LOCK(&ctx->mutex);
    AIRY_MEMCPY(stats, &ctx->stats, sizeof(llm_router_stats_t));
    AIRY_MUTEX_UNLOCK(&ctx->mutex);

    AIRY_LOG_DEBUG("C-L02: LLMRouter: stats queried (total=%llu, cost=$%.6f)",
                   (unsigned long long)stats->total_requests, stats->total_cost);

    return 0;
}
