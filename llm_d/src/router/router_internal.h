/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file router_internal.h
 * @brief Router internal interface - routing-strategy function decls.
 *
 * Called by llm_router.c (the orchestrator) to invoke each strategy impl.
 *
 */

#ifndef AIRY_RT_LLM_ROUTER_INTERNAL_H
#define AIRY_RT_LLM_ROUTER_INTERNAL_H

#include "router/llm_router.h"

#ifdef __cplusplus
extern "C" {
#endif


int route_cost_aware(const llm_route_request_t *request, llm_route_result_t *result);


int route_round_robin(const llm_route_request_t *request, llm_route_result_t *result);


int route_least_latency(const llm_route_request_t *request, llm_route_result_t *result);


int route_quality_first(const llm_route_request_t *request, llm_route_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_ROUTER_INTERNAL_H */