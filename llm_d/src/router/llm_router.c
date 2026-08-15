// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file llm_router.c
 * @brief P3.1.5: LLM router orchestrator - unified select + endpoint mgmt + stats.
 *
 * Orchestrates the four routing strategies and provides the unified
 * airy_router_select_provider() interface. Handles endpoint
 * register/unregister, statistics collection and default-strategy
 * management.
 *
 * Routing strategies (implemented in separate files):
 *   - P3.1.1 cost_aware_router.c:     cost-aware routing (decision tree)
 *   - P3.1.2 round_robin_router.c:    round-robin routing
 *   - P3.1.3 least_latency_router.c:  least-latency routing
 *   - P3.1.4 quality_first_router.c:  quality-first routing
 *
 */

#include "router/router_context.h"
#include "router/router_internal.h"
#include "airy_memory.h"

static router_ctx_t g_router;

router_ctx_t *router_ctx_get(void)
{
    return &g_router;
}

int llm_router_init(const char *config_path)
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

    pricing_rule_t default_rules[] = {
        {"gpt-4*", 0.03, 0.06},          {"gpt-3.5*", 0.001, 0.002},  {"claude*", 0.015, 0.075},
        {"deepseek*", 0.00014, 0.00028}, {"gemini*", 0.0005, 0.0015},
    };
    ctx->cost_tracker =
        cost_tracker_create(default_rules, sizeof(default_rules) / sizeof(default_rules[0]));
    if (!ctx->cost_tracker) {
        AIRY_LOG_ERROR("C-L02: LLMRouter: failed to create cost_tracker STACK: llm_router_init");
        AIRY_MUTEX_DESTROY(&ctx->mutex);
        return AIRY_ERR_GENERIC_FAIL;
    }
    AIRY_LOG_INFO("C-L02: LLMRouter: cost_tracker initialized with %zu pricing rules",
                  sizeof(default_rules) / sizeof(default_rules[0]));

    ctx->token_counter = token_counter_create("cl100k_base");
    if (!ctx->token_counter) {
        AIRY_LOG_WARN("C-L02: LLMRouter: token_counter creation failed, "
                      "will use heuristic estimation");
    } else {
        AIRY_LOG_INFO("C-L02: LLMRouter: token_counter initialized (encoding=cl100k_base)");
    }

    ctx->initialized = true;

    AIRY_LOG_INFO("C-L02: LLMRouter: initialization complete (config=%s)",
                  config_path ? config_path : "default");
    (void)config_path;
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

    if (ctx->cost_tracker) {
        cost_tracker_destroy(ctx->cost_tracker);
        ctx->cost_tracker = NULL;
    }
    if (ctx->token_counter) {
        token_counter_destroy(ctx->token_counter);
        ctx->token_counter = NULL;
    }

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

    AIRY_MEMCPY(&ctx->endpoints[ctx->endpoint_count], endpoint, sizeof(llm_endpoint_t));
    ctx->endpoint_count++;

    AIRY_LOG_INFO("C-L02: LLMRouter: registered endpoint %s/%s (total=%zu, "
                  "caps=0x%x, cost=$%.6f/$%.6f, latency=%ums)",
                  endpoint->provider_name, endpoint->model_name, ctx->endpoint_count,
                  endpoint->capabilities, endpoint->cost_per_1k_input, endpoint->cost_per_1k_output,
                  endpoint->avg_latency_ms);

    AIRY_MUTEX_UNLOCK(&ctx->mutex);
    return 0;
}

int llm_router_unregister_endpoint(const char *provider_name, const char *model_name)
{
    router_ctx_t *ctx = router_ctx_get();

    if (!provider_name || !model_name) {
        AIRY_LOG_ERROR("C-L02: LLMRouter: unregister_endpoint with NULL params STACK: "
                       "llm_router_unregister_endpoint");
        return AIRY_ERR_INVALID_PARAM;
    }

    AIRY_MUTEX_LOCK(&ctx->mutex);

    for (size_t i = 0; i < ctx->endpoint_count; i++) {
        llm_endpoint_t *ep = &ctx->endpoints[i];
        if (strcmp(ep->provider_name, provider_name) == 0 &&
            strcmp(ep->model_name, model_name) == 0) {
            AIRY_LOG_INFO("C-L02: LLMRouter: unregistering endpoint %s/%s", provider_name,
                          model_name);

            if (i < ctx->endpoint_count - 1) {
                AIRY_MEMCPY(ep, &ctx->endpoints[ctx->endpoint_count - 1], sizeof(llm_endpoint_t));
            }
            ctx->endpoint_count--;
            AIRY_MUTEX_UNLOCK(&ctx->mutex);
            return 0;
        }
    }

    AIRY_LOG_WARN("C-L02: LLMRouter: endpoint %s/%s not found for unregister "
                  "(total_endpoints=%zu) STACK: llm_router_unregister_endpoint",
                  provider_name, model_name, ctx->endpoint_count);
    AIRY_MUTEX_UNLOCK(&ctx->mutex);
    return AIRY_ERR_NOT_FOUND;
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

int llm_router_set_default_strategy(llm_route_strategy_t strategy)
{
    router_ctx_t *ctx = router_ctx_get();

    if (strategy >= LLM_ROUTE_COUNT) {
        AIRY_LOG_ERROR(
            "C-L02: LLMRouter: invalid strategy %d STACK: llm_router_set_default_strategy",
            strategy);
        return AIRY_ERR_INVALID_PARAM;
    }

    AIRY_MUTEX_LOCK(&ctx->mutex);
    llm_route_strategy_t old = ctx->default_strategy;
    ctx->default_strategy = strategy;
    AIRY_MUTEX_UNLOCK(&ctx->mutex);

    AIRY_LOG_INFO("C-L02: LLMRouter: default strategy changed %d -> %d", old, strategy);

    return 0;
}

/**
 * @brief airy_router_select_provider - unified provider-selection interface.
 *
 * Wraps llm_router_route with a simpler API. External callers need not
 * construct the llm_route_request_t struct.
 */
int airy_router_select_provider(const char *prompt, size_t prompt_len, uint32_t required_caps,
                                uint32_t max_tokens, double max_cost, uint32_t max_latency_ms,
                                llm_route_strategy_t strategy, llm_route_result_t *result)
{
    llm_route_request_t request;
    AIRY_MEMSET(&request, 0, sizeof(request));

    request.prompt = prompt;
    request.prompt_len = prompt_len;
    request.required_caps = required_caps;
    request.max_tokens = max_tokens;
    request.max_cost = max_cost;
    request.max_latency_ms = max_latency_ms;
    request.strategy = strategy;

    return llm_router_route(&request, result);
}