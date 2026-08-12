// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file cost_aware_router.c
 * @brief P3.1.1: cost-aware routing - decision-tree routing.
 *
 * Decision tree:
 *   budget check -> task-type decision -> provider selection -> fallback chain
 *
 * Selects the cheapest endpoint among those meeting the capability
 * requirements, honoring budget and latency caps.
 *
 */

#include "router/router_context.h"

/**
 * @brief Decision-tree routing (budget check -> task-type decision ->
 *        provider selection -> fallback chain).
 *
 * Routing flow:
 *   1. Filter endpoints meeting the capability requirements
 *   2. Estimate input/output token counts
 *   3. Walk endpoints, skipping those over budget or latency caps
 *   4. Select the cheapest endpoint
 *   5. Set the runner-up endpoint as fallback
 *
 * @param request Routing request
 * @param result  Routing-result output
 * @return 0 on success, -1 if no eligible endpoint
 */
int route_cost_aware(const llm_route_request_t *request, llm_route_result_t *result)
{
    router_ctx_t *ctx = router_ctx_get();
    (void)ctx;
    llm_endpoint_t *eligible[LLM_ROUTER_MAX_ENDPOINTS];
    size_t eligible_count = router_get_eligible(request, eligible, LLM_ROUTER_MAX_ENDPOINTS);

    if (eligible_count == 0) {
        AIRY_LOG_WARN("C-L02: CostAware: no eligible endpoints found "
                      "(caps=0x%x, preferred=%s)",
                      request->required_caps,
                      request->preferred_provider[0] ? request->preferred_provider : "any");
        return AIRY_ERR_NOT_FOUND;
    }

    AIRY_LOG_DEBUG("C-L02: CostAware: evaluating %zu eligible endpoints", eligible_count);

    size_t input_tokens = router_estimate_tokens(request->prompt, request->prompt_len);
    size_t output_tokens = request->max_tokens > 0 ? request->max_tokens : 1024;

    AIRY_LOG_DEBUG("C-L02: CostAware: estimated tokens input=%zu output=%zu "
                   "budget=$%.6f latency_limit=%ums",
                   input_tokens, output_tokens, request->max_cost, request->max_latency_ms);

    size_t best_idx = 0;
    double best_cost = INFINITY;
    size_t fallback_idx = 0;
    size_t skipped_budget = 0;
    size_t skipped_latency = 0;

    for (size_t i = 0; i < eligible_count; i++) {
        double cost = router_estimate_cost(eligible[i], input_tokens, output_tokens);

        AIRY_LOG_DEBUG("C-L02: CostAware: endpoint[%zu] %s/%s cost=$%.6f "
                       "latency=%ums",
                       i, eligible[i]->provider_name, eligible[i]->model_name, cost,
                       eligible[i]->avg_latency_ms);

        if (request->max_cost > 0 && cost > request->max_cost) {
            AIRY_LOG_DEBUG("C-L02: CostAware: skipping %s/%s — over budget "
                           "($%.6f > $%.6f)",
                           eligible[i]->provider_name, eligible[i]->model_name, cost,
                           request->max_cost);
            skipped_budget++;
            continue;
        }

        if (request->max_latency_ms > 0 && eligible[i]->avg_latency_ms > request->max_latency_ms) {
            AIRY_LOG_DEBUG("C-L02: CostAware: skipping %s/%s — over latency "
                           "(%ums > %ums)",
                           eligible[i]->provider_name, eligible[i]->model_name,
                           eligible[i]->avg_latency_ms, request->max_latency_ms);
            skipped_latency++;
            continue;
        }

        if (cost < best_cost) {
            fallback_idx = best_idx;
            best_idx = i;
            best_cost = cost;
        }
    }

    AIRY_LOG_DEBUG("C-L02: CostAware: filtered %zu budget + %zu latency, "
                   "best_cost=$%.6f",
                   skipped_budget, skipped_latency, best_cost);

    if (best_cost == INFINITY) {
        AIRY_LOG_WARN("C-L02: CostAware: no endpoint within budget/latency constraints "
                      "(skipped_budget=%zu, skipped_latency=%zu, total_eligible=%zu, "
                      "budget=$%.6f, latency_limit=%ums) STACK: route_cost_aware",
                      skipped_budget, skipped_latency, eligible_count, request->max_cost,
                      request->max_latency_ms);

        if (skipped_budget == eligible_count && skipped_budget > 0) {
            AIRY_LOG_WARN("C-L02: CostAware: budget exhausted — all %zu eligible "
                          "endpoints exceed budget=$%.6f STACK: route_cost_aware",
                          eligible_count, request->max_cost);
        }
        return AIRY_ERR_NOT_FOUND;
    }

    llm_endpoint_t *best = eligible[best_idx];
    router_fill_result(result, best, LLM_ROUTE_COST, 90, input_tokens, output_tokens);

    if (eligible_count > 1 && fallback_idx != best_idx) {
        router_set_fallback(result, eligible[fallback_idx]);
        AIRY_LOG_DEBUG("C-L02: CostAware: fallback set to %s/%s",
                       eligible[fallback_idx]->provider_name, eligible[fallback_idx]->model_name);
    }

    AIRY_LOG_INFO("C-L02: CostAware: selected %s/%s cost=$%.6f latency=%ums", best->provider_name,
                  best->model_name, best_cost, best->avg_latency_ms);

    return 0;
}