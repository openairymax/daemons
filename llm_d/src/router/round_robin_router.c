// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file round_robin_router.c
 * @brief P3.1.2: round-robin routing.
 *
 * Selects endpoints in round-robin order among those meeting the
 * capability requirements, spreading requests evenly across endpoints.
 *
 */

#include "router/router_context.h"

/**
 * @brief Round-robin routing - cycle through all capable endpoints.
 *
 * Uses the global round_robin_index counter so each request goes to a
 * different endpoint, achieving load balancing.
 *
 * @param request Routing request
 * @param result  Routing-result output
 * @return 0 on success, -1 if no eligible endpoint
 */
int route_round_robin(const llm_route_request_t *request, llm_route_result_t *result)
{
    router_ctx_t *ctx = router_ctx_get();
    llm_endpoint_t *eligible[LLM_ROUTER_MAX_ENDPOINTS];
    size_t eligible_count = router_get_eligible(request, eligible, LLM_ROUTER_MAX_ENDPOINTS);

    if (eligible_count == 0) {
        AIRY_LOG_WARN("C-L02: RoundRobin: no eligible endpoints "
                      "(caps=0x%x, preferred=%s, total_endpoints=%zu) "
                      "STACK: route_round_robin",
                      request->required_caps,
                      request->preferred_provider[0] ? request->preferred_provider : "any",
                      router_ctx_get()->endpoint_count);
        return AIRY_ERR_NOT_FOUND;
    }

    size_t idx = ctx->round_robin_index % eligible_count;
    ctx->round_robin_index++;

    AIRY_LOG_DEBUG("C-L02: RoundRobin: round_robin_index=%zu -> endpoint[%zu/%zu]",
                   ctx->round_robin_index, idx, eligible_count);

    llm_endpoint_t *ep = eligible[idx];
    size_t input_tokens = router_estimate_tokens(request->prompt, request->prompt_len);
    size_t output_tokens = request->max_tokens > 0 ? request->max_tokens : 1024;

    router_fill_result(result, ep, LLM_ROUTE_ROUND_ROBIN, 70, input_tokens, output_tokens);

    if (eligible_count > 1) {
        size_t fallback_idx = (idx + 1) % eligible_count;
        router_set_fallback(result, eligible[fallback_idx]);
        AIRY_LOG_DEBUG("C-L02: RoundRobin: fallback set to %s/%s",
                       eligible[fallback_idx]->provider_name, eligible[fallback_idx]->model_name);
    }

    AIRY_LOG_INFO("C-L02: RoundRobin: selected %s/%s (round=%zu)", ep->provider_name,
                  ep->model_name, ctx->round_robin_index);

    return 0;
}