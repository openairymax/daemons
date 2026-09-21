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

/** @brief 本地 LRU 缓存统计（字段与 mem_cache_stats_t 同形，便于用户面统一展示）。 */
typedef struct {
    size_t entries;   /**< 当前条目数 */
    size_t capacity;  /**< 条目数上限 */
    size_t hits;      /**< 累计命中 */
    size_t misses;    /**< 累计未命中（未命中 + TTL 过期） */
    size_t evictions; /**< 累计 LRU 淘汰数 */
    double hit_rate;  /**< 命中率 hits/(hits+misses)，无查询时为 0 */
} llm_cache_stats_t;

llm_cache_t *llm_cache_create(size_t capacity, int ttl_sec);
void llm_cache_destroy(llm_cache_t *cache);
int llm_cache_get(llm_cache_t *cache, const char *key, char **out_value);
void llm_cache_put(llm_cache_t *cache, const char *key, const char *value);
void llm_cache_clear(llm_cache_t *cache);

size_t llm_cache_size(llm_cache_t *cache);
size_t llm_cache_capacity(llm_cache_t *cache);
void llm_cache_stats(llm_cache_t *cache, llm_cache_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_CACHE_H */