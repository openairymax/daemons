/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file cache.h
 * @brief LRU cache interface.
 */

#ifndef AIRY_RT_LLM_CACHE_H
#define AIRY_RT_LLM_CACHE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct llm_cache llm_cache_t;

llm_cache_t *llm_cache_create(size_t capacity, int ttl_sec);
void llm_cache_destroy(llm_cache_t *cache);
int llm_cache_get(llm_cache_t *cache, const char *key, char **out_value);
void llm_cache_put(llm_cache_t *cache, const char *key, const char *value);
void llm_cache_clear(llm_cache_t *cache);

size_t llm_cache_size(llm_cache_t *cache);
size_t llm_cache_capacity(llm_cache_t *cache);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_CACHE_H */