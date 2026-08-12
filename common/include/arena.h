/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file arena.h
 * @brief P1.19: Arena linear allocator - chained blocks + whole reset.
 *
 * For bulk allocation of short-lived objects. Block-based linear
 * allocator; when a block is exhausted a new one is chained. arena_reset()
 * releases all memory at once.
 *
 * Design goals:
 *   - O(1) allocation (pointer bump only)
 *   - O(1) bulk release (reset the whole arena)
 *   - Zero fragmentation (linear allocation, no frees)
 *   - Not thread-safe (caller locks or uses tcache)
 *
 * @see tcache.h  per-thread caching layer
 */

#ifndef AIRY_RT_ARENA_H
#define AIRY_RT_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct arena_s arena_t;


typedef struct {
    size_t block_size;
    size_t alignment;
    bool use_huge_pages;
} arena_config_t;


/**
 * @brief Arena position mark.
 *
 * Records the current allocation position for later rollback.
 */
typedef struct {
    void *block_start;
    size_t offset;
    void *arena_internal;
} arena_mark_t;


/**
 * @brief Create an arena allocator.
 *
 * @param config Config (NULL = defaults)
 * @return Arena handle, NULL on failure
 */
arena_t *arena_create(const arena_config_t *config);

/**
 * @brief Destroy an arena allocator.
 *
 * Releases all blocks and metadata.
 *
 * @param arena Arena handle
 */
void arena_destroy(arena_t *arena);


/**
 * @brief Allocate memory from the arena.
 *
 * If the current block has insufficient space, a new block is chained.
 *
 * @param arena Arena handle
 * @param size Allocation size
 * @return Memory pointer, NULL on failure
 */
void *arena_alloc(arena_t *arena, size_t size);

/**
 * @brief Allocate aligned memory from the arena.
 *
 * @param arena Arena handle
 * @param size Allocation size
 * @param alignment Alignment requirement (must be a power of 2)
 * @return Memory pointer, NULL on failure
 */
void *arena_alloc_aligned(arena_t *arena, size_t size, size_t alignment);

/**
 * @brief Allocate zeroed memory from the arena.
 *
 * @param arena Arena handle
 * @param size Allocation size
 * @return Memory pointer, NULL on failure
 */
void *arena_calloc(arena_t *arena, size_t size);

/**
 * @brief Reset the arena.
 *
 * Resets every block's allocation pointer to its start. Block memory is
 * not freed; later allocations reuse existing blocks.
 *
 * @param arena Arena handle
 */
void arena_reset(arena_t *arena);


/**
 * @brief Record the current arena position.
 *
 * @param arena Arena handle
 * @return Position mark
 */
arena_mark_t arena_mark(arena_t *arena);

/**
 * @brief Roll back to a mark position.
 *
 * Releases all blocks allocated after the mark and rewinds the current
 * block pointer to the mark. Newly-chained blocks after the mark are freed.
 *
 * @param arena Arena handle
 * @param mark Position mark
 */
void arena_rollback(arena_t *arena, arena_mark_t mark);


/**
 * @brief Get arena statistics.
 *
 * @param arena Arena handle
 * @param out_total_allocated Total allocated amount
 * @param out_total_blocks Total block count
 * @param out_current_usage Current usage
 * @param out_peak_usage Peak usage
 */
void arena_get_stats(arena_t *arena, size_t *out_total_allocated, size_t *out_total_blocks,
                     size_t *out_current_usage, size_t *out_peak_usage);

/**
 * @brief Get the remaining space in the current block.
 *
 * @param arena Arena handle
 * @return Remaining bytes
 */
size_t arena_available(arena_t *arena);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_ARENA_H */