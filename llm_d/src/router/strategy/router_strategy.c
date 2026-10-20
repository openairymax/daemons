// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file router_strategy.c
 * @brief Routing strategies - shared skeleton plus four selectors.
 *
 * Mechanism (strategy_begin/strategy_tokens/strategy_finish): eligible
 * collection, token estimation, result fill, fallback and logging. Policy:
 * each route_* implements only its selection rule.
 *
 * Selection rules:
 *   cost_aware    - cheapest within budget/latency caps; runner-up fallback
 *   round_robin   - cyclic counter; next-index fallback
 *   least_latency - lowest avg latency; second-lowest fallback
 *   quality_first - priority then context_window; next-ranked fallback
 *
 */

#include "router/core/router_context.h"

#include <stdlib.h>

/* 收集满足能力要求的端点；为空时告警并返回 0。 */
static size_t strategy_begin(const char *tag, const llm_route_request_t *request,
                             llm_endpoint_t *eligible[])
{
    size_t count = router_get_eligible(request, eligible, LLM_ROUTER_MAX_ENDPOINTS);
    if (count == 0) {
        AIRY_LOG_WARN("C-L02: %s: no eligible endpoints (caps=0x%x, preferred=%s, "
                      "total=%zu) STACK: router_strategy",
                      tag, request->required_caps,
                      request->preferred_provider[0] ? request->preferred_provider : "any",
                      router_ctx_get()->endpoint_count);
    }
    return count;
}

/* 估算输入/输出 token 数。 */
static void strategy_tokens(const llm_route_request_t *req, size_t *in, size_t *out)
{
    *in = router_estimate_tokens(req->prompt, req->prompt_len);
    *out = req->max_tokens > 0 ? req->max_tokens : LLM_ROUTE_EST_OUTPUT_TOKENS;
}

/* 填充结果与备选端点，输出统一格式的选中日志。 */
static int strategy_finish(llm_route_result_t *result, llm_endpoint_t *best,
                           llm_endpoint_t *fallback, llm_route_strategy_t type,
                           int score, const char *tag, size_t in, size_t out)
{
    router_fill_result(result, best, type, score, in, out);
    if (fallback && fallback != best)
        router_set_fallback(result, fallback);
    AIRY_LOG_INFO("C-L02: %s: selected %s/%s cost=$%.6f latency=%ums conf=%d",
                  tag, best->provider_name, best->model_name, result->estimated_cost,
                  result->estimated_latency_ms, score);
    return 0;
}

/* cost-aware - cheapest endpoint within budget and latency caps. */
int route_cost_aware(const llm_route_request_t *request, llm_route_result_t *result)
{
    llm_endpoint_t *eligible[LLM_ROUTER_MAX_ENDPOINTS];
    size_t count = strategy_begin("CostAware", request, eligible);
    if (count == 0)
        return AIRY_ERR_NOT_FOUND;

    size_t in, out;
    strategy_tokens(request, &in, &out);

    size_t best_idx = 0, fb_idx = 0, skip_budget = 0, skip_latency = 0;
    double best_cost = INFINITY;
    for (size_t i = 0; i < count; i++) {
        double cost = router_estimate_cost(eligible[i], in, out);
        AIRY_LOG_DEBUG("C-L02: CostAware: [%zu] %s/%s cost=$%.6f latency=%ums", i,
                       eligible[i]->provider_name, eligible[i]->model_name, cost,
                       eligible[i]->avg_latency_ms);
        if (request->max_cost > 0 && cost > request->max_cost) {
            skip_budget++;
        } else if (request->max_latency_ms > 0 &&
                   eligible[i]->avg_latency_ms > request->max_latency_ms) {
            skip_latency++;
        } else if (cost < best_cost) {
            fb_idx = best_idx;
            best_idx = i;
            best_cost = cost;
        }
    }
    AIRY_LOG_DEBUG("C-L02: CostAware: skipped budget=%zu latency=%zu best=$%.6f",
                   skip_budget, skip_latency, best_cost);
    if (best_cost == INFINITY) {
        AIRY_LOG_WARN("C-L02: CostAware: no endpoint within constraints "
                      "(budget_skipped=%zu latency_skipped=%zu eligible=%zu "
                      "budget=$%.6f latency<=%ums) STACK: route_cost_aware",
                      skip_budget, skip_latency, count, request->max_cost,
                      request->max_latency_ms);
        if (skip_budget == count)
            AIRY_LOG_WARN("C-L02: CostAware: budget exhausted - all %zu endpoints "
                          "over $%.6f STACK: route_cost_aware",
                          count, request->max_cost);
        return AIRY_ERR_NOT_FOUND;
    }

    llm_endpoint_t *fb = fb_idx != best_idx ? eligible[fb_idx] : NULL;
    return strategy_finish(result, eligible[best_idx], fb, LLM_ROUTE_COST, 90,
                           "CostAware", in, out);
}

/* round-robin - cyclic counter over eligible endpoints. */
int route_round_robin(const llm_route_request_t *request, llm_route_result_t *result)
{
    router_ctx_t *ctx = router_ctx_get();
    llm_endpoint_t *eligible[LLM_ROUTER_MAX_ENDPOINTS];
    size_t count = strategy_begin("RoundRobin", request, eligible);
    if (count == 0)
        return AIRY_ERR_NOT_FOUND;

    size_t idx = ctx->round_robin_index % count;
    ctx->round_robin_index++;
    AIRY_LOG_DEBUG("C-L02: RoundRobin: index=%zu -> endpoint[%zu/%zu]",
                   ctx->round_robin_index, idx, count);

    size_t in, out;
    strategy_tokens(request, &in, &out);
    llm_endpoint_t *fb = count > 1 ? eligible[(idx + 1) % count] : NULL;
    return strategy_finish(result, eligible[idx], fb, LLM_ROUTE_ROUND_ROBIN, 70,
                           "RoundRobin", in, out);
}

/* least-latency - lowest avg latency; second-lowest fallback. */
int route_least_latency(const llm_route_request_t *request, llm_route_result_t *result)
{
    llm_endpoint_t *eligible[LLM_ROUTER_MAX_ENDPOINTS];
    size_t count = strategy_begin("Latency", request, eligible);
    if (count == 0)
        return AIRY_ERR_NOT_FOUND;

    size_t best_idx = 0, fb_idx = 0;
    uint32_t best = UINT32_MAX, second = UINT32_MAX;
    for (size_t i = 0; i < count; i++) {
        uint32_t latency = eligible[i]->avg_latency_ms;
        AIRY_LOG_DEBUG("C-L02: Latency: [%zu] %s/%s latency=%ums", i,
                       eligible[i]->provider_name, eligible[i]->model_name, latency);
        if (latency < best) {
            second = best;
            fb_idx = best_idx;
            best = latency;
            best_idx = i;
        } else if (latency < second) {
            second = latency;
            fb_idx = i;
        }
    }
    AIRY_LOG_DEBUG("C-L02: Latency: range min=%ums second=%ums across %zu endpoints",
                   best, second, count);

    size_t in, out;
    strategy_tokens(request, &in, &out);
    return strategy_finish(result, eligible[best_idx], eligible[fb_idx],
                           LLM_ROUTE_LATENCY, 85, "Latency", in, out);
}

/* quality_cmp: priority desc, then context_window desc (total order). */
static int quality_cmp(const void *a, const void *b)
{
    const llm_endpoint_t *x = *(llm_endpoint_t *const *)a;
    const llm_endpoint_t *y = *(llm_endpoint_t *const *)b;
    if (x->priority != y->priority)
        return x->priority > y->priority ? -1 : 1;
    if (x->context_window != y->context_window)
        return x->context_window > y->context_window ? -1 : 1;
    return 0;
}

/* quality-first - priority then context window. */
int route_quality_first(const llm_route_request_t *request, llm_route_result_t *result)
{
    llm_endpoint_t *eligible[LLM_ROUTER_MAX_ENDPOINTS];
    size_t count = strategy_begin("Quality", request, eligible);
    if (count == 0)
        return AIRY_ERR_NOT_FOUND;

    qsort(eligible, count, sizeof(eligible[0]), quality_cmp);
    AIRY_LOG_DEBUG("C-L02: Quality: ranked %zu endpoints, top=%s/%s prio=%d ctx=%u",
                   count, eligible[0]->provider_name, eligible[0]->model_name,
                   eligible[0]->priority, eligible[0]->context_window);

    size_t in, out;
    strategy_tokens(request, &in, &out);
    llm_endpoint_t *fb = count > 1 ? eligible[1] : NULL;
    return strategy_finish(result, eligible[0], fb, LLM_ROUTE_COMPLEXITY, 95,
                           "Quality", in, out);
}
