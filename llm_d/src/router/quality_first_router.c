// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file quality_first_router.c
 * @brief P3.1.4: quality-first routing.
 *
 * Sorts by priority (higher priority first); ties broken by descending
 * context-window size, selecting the highest-quality endpoint.
 *
 */

#include "router/router_context.h"

/**
 * @brief Quality-first routing - pick the best endpoint by priority and
 *        context-window size.
 *
 * Sort rules:
 *   1. Higher priority wins
 *   2. On equal priority, larger context_window wins
 *
 * For scenarios needing high-quality output (e.g. code generation,
 * complex reasoning).
 *
 * @param request Routing request
 * @param result  Routing-result output
 * @return 0 on success, -1 if no eligible endpoint
 */
int route_quality_first(const llm_route_request_t *request, llm_route_result_t *result)
{
    router_ctx_t *ctx = router_ctx_get();
    (void)ctx;
    llm_endpoint_t *eligible[LLM_ROUTER_MAX_ENDPOINTS];
    size_t eligible_count = router_get_eligible(request, eligible, LLM_ROUTER_MAX_ENDPOINTS);

    if (eligible_count == 0) {
        AIRY_LOG_WARN("C-L02: Quality: no eligible endpoints "
                      "(caps=0x%x, total_endpoints=%zu) STACK: route_quality_first",
                      request->required_caps, router_ctx_get()->endpoint_count);
        return AIRY_ERR_NOT_FOUND;
    }

    AIRY_LOG_DEBUG("C-L02: Quality: sorting %zu endpoints by quality", eligible_count);

    for (size_t i = 0; i < eligible_count; i++) {
        for (size_t j = i + 1; j < eligible_count; j++) {
            bool should_swap = false;
            if (eligible[i]->priority < eligible[j]->priority) {
                should_swap = true;
            } else if (eligible[i]->priority == eligible[j]->priority &&
                       eligible[i]->context_window < eligible[j]->context_window) {
                should_swap = true;
            }
            if (should_swap) {
                llm_endpoint_t *tmp = eligible[i];
                eligible[i] = eligible[j];
                eligible[j] = tmp;
            }
        }
    }

    for (size_t i = 0; i < eligible_count; i++) {
        AIRY_LOG_DEBUG("C-L02: Quality: rank[%zu] %s/%s priority=%d "
                       "context=%u",
                       i, eligible[i]->provider_name, eligible[i]->model_name,
                       eligible[i]->priority, eligible[i]->context_window);
    }

    AIRY_LOG_DEBUG("C-L02: Quality: priority range max=%d min=%d "
                   "across %zu endpoints",
                   eligible[0]->priority, eligible[eligible_count - 1]->priority, eligible_count);

    llm_endpoint_t *ep = eligible[0];
    size_t input_tokens = router_estimate_tokens(request->prompt, request->prompt_len);
    size_t output_tokens = request->max_tokens > 0 ? request->max_tokens : 1024;

    router_fill_result(result, ep, LLM_ROUTE_COMPLEXITY, 95, input_tokens, output_tokens);

    if (eligible_count > 1) {
        router_set_fallback(result, eligible[1]);
        AIRY_LOG_DEBUG("C-L02: Quality: fallback set to %s/%s "
                       "(priority=%d)",
                       eligible[1]->provider_name, eligible[1]->model_name, eligible[1]->priority);
    }

    AIRY_LOG_INFO("C-L02: Quality: selected %s/%s priority=%d "
                  "context=%u",
                  ep->provider_name, ep->model_name, ep->priority, ep->context_window);

    return 0;
}