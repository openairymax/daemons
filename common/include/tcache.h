/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tcache.h
 * @brief P1.20: per-thread caching layer - batch acquire/return + cap.
 *
 * Maintains a small allocation cache in each thread's TLS. Small-object
 * allocations are served from the tcache, avoiding frequent access to the
 * global allocator. When the cache is exhausted, objects are fetched in
 * batch from the Arena or the global allocator.
 *
 * Design goals:
 *   - Single-thread allocation latency reduced by > 30%
 *   - Batch acquire/return reduces lock contention
 *   - Cap prevents a single thread from over-caching
 *
 * @see arena.h  Arena linear allocator
 */

#ifndef AIRY_RT_TCACHE_H
#define AIRY_RT_TCACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct tcache_s tcache_t;


typedef struct {
    size_t max_cache_entries;
    size_t batch_fill_count;
    uint32_t max_cache_size_class;
    uint32_t max_total_cached_bytes;
    bool enable_stats;
} tcache_config_t;


typedef struct {
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t cache_hit_count;
    uint64_t cache_miss_count;
    uint64_t batch_fill_count;
    uint64_t batch_flush_count;
    uint64_t oversized_alloc;
    double hit_rate;
} tcache_stats_t;


/**
 * @brief Create a tcache instance.
 *
 * Each thread should create its own tcache instance (typically stored in
 * _Thread_local).
 *
 * @param config Config (NULL = defaults)
 * @return tcache handle, NULL on failure
 */
tcache_t *tcache_create(const tcache_config_t *config);

/**
 * @brief Destroy a tcache instance.
 *
 * Returns all cached objects to the global allocator.
 *
 * @param tc tcache handle
 */
void tcache_destroy(tcache_t *tc);


/**
 * @brief Allocate memory from the tcache.
 *
 * Serves from the thread-local cache first; on a miss, fetches in batch
 * from the global allocator. Objects above max_cache_size_class are
 * allocated directly.
 *
 * @param tc tcache handle
 * @param size Allocation size
 * @return Memory pointer, NULL on failure
 */
void *tcache_alloc(tcache_t *tc, size_t size);

/**
 * @brief Free memory to the tcache.
 *
 * Small objects are cached thread-locally; large objects are freed
 * directly. If the cache is full, objects are returned in batch to the
 * global allocator.
 *
 * @param tc tcache handle
 * @param ptr Memory pointer
 * @param size Size used at allocation
 */
void tcache_free(tcache_t *tc, void *ptr, size_t size);


/**
 * @brief Return all cached objects to the global allocator in batch.
 *
 * Call before the thread exits to ensure all cached objects are released.
 *
 * @param tc tcache handle
 */
void tcache_flush(tcache_t *tc);

/**
 * @brief Clear the cache (without returning to the global allocator).
 *
 * Used in Arena mode: cached objects belong to the Arena and need no
 * separate release.
 *
 * @param tc tcache handle
 */
void tcache_purge(tcache_t *tc);


/**
 * @brief Get tcache statistics.
 *
 * @param tc tcache handle
 * @param out_stats Output statistics
 */
void tcache_get_stats(tcache_t *tc, tcache_stats_t *out_stats);

/**
 * @brief Get the number of currently cached objects.
 *
 * @param tc tcache handle
 * @return Cached object count
 */
size_t tcache_cached_count(tcache_t *tc);

/**
 * @brief Get the total bytes currently cached.
 *
 * @param tc tcache handle
 * @return Cached bytes
 */
size_t tcache_cached_bytes(tcache_t *tc);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TCACHE_H */