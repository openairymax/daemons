/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_service_internal.h
 * @brief Internal declarations shared across the LLM service files.
 */

#ifndef AIRY_RT_LLM_SERVICE_SPLIT_INTERNAL_H
#define AIRY_RT_LLM_SERVICE_SPLIT_INTERNAL_H

#include "service.h"
#include "cost_tracker.h"
#include "providers/registry.h"

#include <cjson/cJSON.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Config-loading domain (service_config.c) ---- */

int ends_with(const char *str, const char *suffix);
pricing_rule_t *load_pricing_rules(cJSON *root, int *count);
void free_pricing_rules(pricing_rule_t *rules, int count);
int load_pricing_rules_from_yaml(const char *config_path, pricing_rule_t **out_rules,
                                 int *out_count);
int svc_load_model_config(const char *config_path, provider_config_t **out_providers,
                          size_t *out_count);

/* ---- Provider-management domain (service_providers.c) ---- */

void free_provider_configs(provider_config_t *providers, size_t count);
void merge_provider_configs(const provider_config_t *main_provs, size_t main_cnt,
                            const provider_config_t *user_provs, size_t user_cnt,
                            provider_config_t **out, size_t *out_cnt);
void register_router_endpoints(llm_service_t *svc);

/* ---- Complexity-evaluation and statistics domain (service_metrics.c) ---- */

/**
 * @brief Complexity assessment levels (BAN-133 coding contract)
 */
typedef enum {
    LLM_COMPLEXITY_SIMPLE = 0,
    LLM_COMPLEXITY_MODERATE = 1,
    LLM_COMPLEXITY_COMPLEX = 2
} llm_complexity_level_t;

llm_complexity_level_t assess_complexity(const char *input);
void log_routing_decision(const char *model, llm_complexity_level_t complexity,
                          size_t input_len, const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_SERVICE_SPLIT_INTERNAL_H */
