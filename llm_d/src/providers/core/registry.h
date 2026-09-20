/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file registry.h
 * @brief Provider registry interface.
 */

#ifndef AIRY_RT_LLM_PROVIDER_REGISTRY_H
#define AIRY_RT_LLM_PROVIDER_REGISTRY_H

#include "adapter.h"

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
    /* 与 models 同下标对齐的每模型输出上限（model.yaml max_output 的 token
     * 数，0 = 未配置）。NULL 表示调用方未提供容量表。生成参数解析需要它来
     * 兑现"配置了上限就按下限截断"，此前该信息止步于 YAML 解析状态机。 */
    const int *model_max_output;
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

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_PROVIDER_REGISTRY_H */