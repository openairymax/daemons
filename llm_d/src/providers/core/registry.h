/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file registry.h
 * @brief Provider registry interface.
 */

#ifndef AIRY_RT_LLM_PROVIDER_REGISTRY_H
#define AIRY_RT_LLM_PROVIDER_REGISTRY_H

#include "adapter.h"
#include "provider_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct provider_registry provider_registry_t;

provider_registry_t *provider_registry_create(const service_config_t *cfg);
void provider_registry_destroy(provider_registry_t *reg);
const provider_t *provider_registry_find(provider_registry_t *reg, const char *model);

/**
 * @brief Output-token cap configured for a model (model.yaml max_output).
 *
 * @param prov  Provider record (may be NULL)
 * @param model Model name (may be NULL)
 * @return Configured cap in tokens; 0 when the provider/model is unknown or
 *         declares no cap
 */
int provider_registry_model_max_output(const provider_t *prov, const char *model);

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

/**
 * @brief Visit every provider record in the registry.
 *
 * Single traversal primitive of the registry: provider_registry_enumerate is
 * a thin adapter over it. Callers that need provider identity beyond the name
 * (e.g. the endpoint base URL behind an endpoint health probe) use this form
 * directly; for each registered provider the callback receives the immutable
 * provider_t record.
 *
 * @param reg Registry
 * @param cb  Callback (must not be NULL); iteration stops when it returns non-zero
 * @param user_data User data passed through to the callback
 * @return Non-zero if the iteration was short-circuited by the callback;
 *         0 if reg/cb is NULL or the walk ran to completion
 */
int provider_reg_walk(provider_registry_t *reg, int (*cb)(const provider_t *prov, void *user_data),
                      void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_PROVIDER_REGISTRY_H */
