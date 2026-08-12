/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file router_context.h
 * @brief Router shared context - global state, helpers, endpoint management.
 *
 * Shared by all router implementation files to avoid circular deps.
 *
 */

#ifndef AIRY_RT_LLM_ROUTER_CONTEXT_H
#define AIRY_RT_LLM_ROUTER_CONTEXT_H

#include "router/llm_router.h"
#include "cost_tracker.h"
#include "token_counter.h"
#include "airy_memory.h"
/* d8 cleanup: removed sync_compat.h (this file only uses airy_mtx_t,
 * obtained transitively via airy_memory.h -> error.h -> types.h ->
 * platform.h; sync_compat.h is not needed) */

#include "platform.h"

#include "logging_compat.h"
#include "logging.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif


#define LLM_ROUTER_MAX_ENDPOINTS 64
#define LLM_ROUTER_MAX_FALLBACK 3


typedef struct {
    llm_endpoint_t endpoints[LLM_ROUTER_MAX_ENDPOINTS];
    size_t endpoint_count;
    llm_route_strategy_t default_strategy;
    llm_router_stats_t stats;
    cost_tracker_t *cost_tracker;
    token_counter_t *token_counter;
    airy_mtx_t mutex;
    bool initialized;


    size_t round_robin_index;
} router_ctx_t;

/** @brief Get the global router context. */
router_ctx_t *router_ctx_get(void);


/** @brief Estimate the token count of a request. */
static inline size_t router_estimate_tokens(const char *prompt, size_t prompt_len)
{
    router_ctx_t *ctx = router_ctx_get();
    if (!ctx->token_counter || !prompt)
        return prompt_len / 4;

    return token_counter_count(ctx->token_counter, prompt);
}

/** @brief Compute the endpoint cost. */
static inline double router_estimate_cost(const llm_endpoint_t *ep, size_t input_tokens,
                                          size_t output_tokens)
{
    return (ep->cost_per_1k_input * input_tokens / 1000.0) +
           (ep->cost_per_1k_output * output_tokens / 1000.0);
}

/** @brief Check whether an endpoint satisfies the capability requirement. */
static inline bool router_has_capabilities(const llm_endpoint_t *ep, uint32_t required_caps)
{
    return (ep->capabilities & required_caps) == required_caps;
}

/** @brief Get the list of capability-matching endpoints. */
static inline size_t router_get_eligible(const llm_route_request_t *request,
                                         llm_endpoint_t **out_array, size_t max_count)
{
    router_ctx_t *ctx = router_ctx_get();
    size_t count = 0;
    for (size_t i = 0; i < ctx->endpoint_count && count < max_count; i++) {
        llm_endpoint_t *ep = &ctx->endpoints[i];
        if (!ep->enabled)
            continue;
        if (!router_has_capabilities(ep, request->required_caps))
            continue;
        if (request->preferred_provider[0] &&
            strcmp(ep->provider_name, request->preferred_provider) != 0)
            continue;
        out_array[count++] = ep;
    }
    return count;
}

/** @brief Fill the base fields of a routing result. */
static inline void router_fill_result(llm_route_result_t *result, const llm_endpoint_t *ep,
                                      llm_route_strategy_t strategy, int confidence,
                                      size_t input_tokens, size_t output_tokens)
{
    AIRY_STRNCPY_TERM(result->provider_name, ep->provider_name, sizeof(result->provider_name));
    AIRY_STRNCPY_TERM(result->model_name, ep->model_name, sizeof(result->model_name));
    AIRY_STRNCPY_TERM(result->endpoint, ep->endpoint, sizeof(result->endpoint));
    result->estimated_cost = router_estimate_cost(ep, input_tokens, output_tokens);
    result->estimated_latency_ms = ep->avg_latency_ms;
    result->strategy_used = strategy;
    result->confidence = confidence;
}

/** @brief Set the fallback endpoint. */
static inline void router_set_fallback(llm_route_result_t *result, const llm_endpoint_t *fallback)
{
    if (!fallback)
        return;
    AIRY_STRNCPY_TERM(result->fallback_provider, fallback->provider_name,
                      sizeof(result->fallback_provider));
    AIRY_STRNCPY_TERM(result->fallback_model, fallback->model_name, sizeof(result->fallback_model));
}

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_ROUTER_CONTEXT_H */