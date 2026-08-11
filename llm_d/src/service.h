/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service.h
 * @brief 服务内部结构声明
 */

#ifndef AIRY_RT_LLM_SERVICE_INTERNAL_H
#define AIRY_RT_LLM_SERVICE_INTERNAL_H

#include "cache.h"
#include "cost_tracker.h"
#include "llm_service.h"
#include "daemon_platform_ext.h"
#include "providers/registry.h"
#include "token_counter.h"

struct llm_service {
    provider_registry_t *registry;
    llm_cache_t *cache;
    cost_tracker_t *cost;
    token_counter_t *token_counter;
    airy_mtx_t lock;
    void *rules;
    size_t rule_count;
    char default_model[128];
    char default_provider[64]; /* global.default_provider */
};

#endif /* AIRY_RT_LLM_SERVICE_INTERNAL_H */