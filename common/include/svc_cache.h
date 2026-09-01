/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_cache.h
 * @brief Cache-service compatibility layer.
 *
 * Compat layer for agentrt/commons/utils/cache providing backward
 * compatible APIs. New code should use #include <cache_common.h>.
 *
 * @see agentrt/commons/utils/cache/cache_common.h
 */

#ifndef SVC_CACHE_H
#define SVC_CACHE_H


#include <cache_common.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


/** @brief Backward-compatible cache type name.
 * @deprecated use cache_t
 */
typedef cache_t svc_cache_t;

/** @brief Backward-compatible cache config struct.
 * @deprecated use cache_config_t
 */
typedef struct {
    size_t capacity;
    int ttl_sec;
    cache_free_func_t value_free_fn;
} svc_cache_config_t;


/** @brief Create a cache (compat layer).
 * @param manager Cache config
 * @return Cache pointer, NULL on failure
 * @deprecated use cache_create()
 */
static inline svc_cache_t *svc_cache_create(const svc_cache_config_t *manager)
{
    if (!manager) {
        return NULL;
    }

    cache_config_t config = cache_create_default_config();
    config.capacity = manager->capacity;
    config.ttl_sec = manager->ttl_sec;
    config.value_free_func = manager->value_free_fn;

    return cache_create(&config);
}

/** @brief Destroy a cache (compat layer).
 * @param cache Cache pointer
 * @deprecated use cache_destroy()
 */
static inline void svc_cache_destroy(svc_cache_t *cache)
{
    cache_destroy(cache);
}

/** @brief Get a cache item (compat layer).
 * @param cache Cache pointer
 * @param key Cache key
 * @param out_value Output value
 * @return 1 hit, 0 miss, negative on error
 * @deprecated use cache_get()
 */
static inline int svc_cache_get(svc_cache_t *cache, const char *key, void **out_value)
{
    return cache_get(cache, key, out_value);
}

/** @brief Put a cache item (compat layer).
 * @param cache Cache pointer
 * @param key Cache key
 * @param value Cache value
 * @param value_size Value size (ignored here; values stored as pointers)
 * @return 0 on success, non-zero on error
 * @deprecated use cache_put()
 */
static inline int svc_cache_put(svc_cache_t *cache, const char *key, const void *value,
                                size_t value_size __attribute__((unused)))
{
    cache_put(cache, key, value);
    return 0;
}

/** @brief Clear all cache items (compat layer).
 * @param cache Cache pointer
 * @deprecated use cache_clear()
 */
static inline void svc_cache_clear(svc_cache_t *cache)
{
    cache_clear(cache);
}

/** @brief Get the number of cache items (compat layer).
 * @param cache Cache pointer
 * @return Item count
 * @deprecated use cache_get_size()
 */
static inline size_t svc_cache_size(svc_cache_t *cache)
{
    return cache_get_size(cache);
}

/** @brief Check whether the cache is empty (compat layer).
 * @param cache Cache pointer
 * @return true if empty
 * @deprecated use cache_get_size() == 0
 */
static inline bool svc_cache_is_empty(svc_cache_t *cache)
{
    return cache_get_size(cache) == 0;
}


/** @brief Check whether the cache contains a key.
 * @param cache Cache pointer
 * @param key Cache key
 * @return true if present
 */
static inline bool svc_cache_contains(svc_cache_t *cache, const char *key)
{
    void *value = NULL;
    int result = cache_get(cache, key, &value);
    if (result == 1 && value) {

        AIRY_FREE(value);
        return true;
    }
    return false;
}

/** @brief Get the cache capacity.
 * @param cache Cache pointer
 * @return Cache capacity
 */
static inline size_t svc_cache_capacity(svc_cache_t *cache)
{
    return cache_get_capacity(cache);
}

/** @brief Set the cache capacity.
 * @param cache Cache pointer
 * @param capacity New capacity
 */
static inline void svc_cache_set_capacity(svc_cache_t *cache, size_t capacity)
{
    cache_set_capacity(cache, capacity);
}

/** @brief Get the cache TTL.
 * @param cache Cache pointer
 * @return TTL in seconds
 */
static inline int svc_cache_get_ttl(svc_cache_t *cache)
{
    return cache_get_ttl(cache);
}

/** @brief Set the cache TTL.
 * @param cache Cache pointer
 * @param ttl_sec TTL in seconds
 */
static inline void svc_cache_set_ttl(svc_cache_t *cache, int ttl_sec)
{
    cache_set_ttl(cache, ttl_sec);
}

#ifdef __cplusplus
}
#endif

#endif /* SVC_CACHE_H */
