/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file registry.h
 * @brief Provider registry interface.
 */

#ifndef AIRY_RT_LLM_PROVIDER_REGISTRY_H
#define AIRY_RT_LLM_PROVIDER_REGISTRY_H

#include "provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    const char *api_key;
    const char *api_base;
    const char *organization;
    double timeout_sec;
    int max_retries;
    char **models;
} provider_config_t;

typedef struct service_config {
    size_t llm_cache_capacity;
    uint32_t llm_cache_ttl_sec;
    int max_retries;
    uint32_t timeout_ms;
    const char *token_encoding;
    provider_config_t *providers;
    size_t provider_count;
} service_config_t;

typedef struct provider_registry provider_registry_t;

provider_registry_t *provider_registry_create(const service_config_t *cfg);
provider_registry_t *provider_registry_create_from_config(const service_config_t *cfg,
                                                          const char *config_path);
void provider_registry_destroy(provider_registry_t *reg);
const provider_t *provider_registry_find(provider_registry_t *reg, const char *model);

/**
 * @brief Enumerate all (provider, model) pairs in the registry.
 *
 * P3.16 (ACC-DT17): traversal interface for llm_router endpoint
 * registration. For every model of every registered provider, calls the
 * callback (provider_name, model_name, user_data).
 *
 * @param reg Registry
 * @param cb  Callback (must not be NULL); enumeration stops when it returns non-zero
 * @param user_data User data passed through to the callback
 * @return Number of non-zero callback returns (for short-circuiting);
 *         0 if reg/cb is NULL
 */
int provider_registry_enumerate(provider_registry_t *reg,
                                int (*cb)(const char *provider_name, const char *model_name,
                                          void *user_data),
                                void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_PROVIDER_REGISTRY_H */