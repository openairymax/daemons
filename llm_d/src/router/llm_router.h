/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_router.h
 * @brief LLM router interface.
 *
 * The LLM Router routes LLM requests to the most suitable provider and
 * model based on request characteristics (complexity, cost, latency, etc.).
 *
 * Routing strategies:
 *   - COMPLEXITY_BASED:  route by task complexity
 *   - COST_OPTIMIZED:    cost-optimized routing
 *   - LATENCY_OPTIMIZED: latency-optimized routing
 *   - FALLBACK:          fallback routing (switch when primary fails)
 *   - ROUND_ROBIN:       round-robin routing
 *
 * @owner team-A
 * @see contracts/contract_A_B.h section 3 (protocol routing table)
 */

#ifndef AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_ROUTER_H
#define AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    LLM_ROUTE_COMPLEXITY = 0,
    LLM_ROUTE_COST = 1,
    LLM_ROUTE_LATENCY = 2,
    LLM_ROUTE_FALLBACK = 3,
    LLM_ROUTE_ROUND_ROBIN = 4,
    LLM_ROUTE_COUNT = 5
} llm_route_strategy_t;


typedef enum {
    LLM_CAP_CHAT = 0x0001,
    LLM_CAP_COMPLETION = 0x0002,
    LLM_CAP_EMBEDDING = 0x0004,
    LLM_CAP_FUNCTION_CALL = 0x0008,
    LLM_CAP_VISION = 0x0010,
    LLM_CAP_STREAMING = 0x0020,
    LLM_CAP_JSON_MODE = 0x0040,
    LLM_CAP_EXTENDED_THINK = 0x0080,
    LLM_CAP_CODE_EXEC = 0x0100
} llm_capability_t;


typedef struct {
    char provider_name[64];
    char model_name[64];
    char endpoint[256];
    char api_key_env[64];
    uint32_t capabilities;
    uint32_t context_window;
    double cost_per_1k_input;
    double cost_per_1k_output;
    uint32_t avg_latency_ms;
    uint32_t rate_limit_rpm;
    bool enabled;
    int priority;
} llm_endpoint_t;


typedef struct {
    const char *prompt;
    size_t prompt_len;
    uint32_t required_caps;
    uint32_t max_tokens;
    double max_cost;
    uint32_t max_latency_ms;
    llm_route_strategy_t strategy;
    char preferred_provider[64];
} llm_route_request_t;


typedef struct {
    char provider_name[64];
    char model_name[64];
    char endpoint[256];
    double estimated_cost;
    uint32_t estimated_latency_ms;
    llm_route_strategy_t strategy_used;
    int confidence;
    char fallback_provider[64];
    char fallback_model[64];
} llm_route_result_t;


typedef struct {
    uint64_t total_requests;
    uint64_t routed_count[5];
    uint64_t fallback_count;
    uint64_t error_count;
    double total_cost;
    uint64_t total_tokens;
} llm_router_stats_t;


/**
 * @brief Initialize the LLM router.
 * @param config_path Config file path
 * @return 0 on success, non-zero on failure
 */
int llm_router_init(const char *config_path);

/** @brief Destroy the LLM router. */
void llm_router_destroy(void);

/**
 * @brief Register a provider endpoint.
 * @param endpoint Endpoint info
 * @return 0 on success, non-zero on failure
 */
int llm_router_register_endpoint(const llm_endpoint_t *endpoint);

/**
 * @brief Unregister a provider endpoint.
 * @param provider_name Provider name
 * @param model_name    Model name
 * @return 0 on success, non-zero on failure
 */
int llm_router_unregister_endpoint(const char *provider_name, const char *model_name);

/**
 * @brief Route an LLM request.
 * @param request Routing request
 * @param result  Routing result
 * @return 0 on success, non-zero on failure
 */
int llm_router_route(const llm_route_request_t *request, llm_route_result_t *result);

/**
 * @brief Get router statistics.
 * @param stats Output statistics
 * @return 0 on success, non-zero on failure
 */
int llm_router_get_stats(llm_router_stats_t *stats);

/**
 * @brief Set the default routing strategy.
 * @param strategy Routing strategy
 * @return 0 on success, non-zero on failure
 */
int llm_router_set_default_strategy(llm_route_strategy_t strategy);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_ROUTER_H */
