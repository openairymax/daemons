/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_degradation.h
 * @brief Daemon-layer graceful degradation handlers (SEC-14 compliant).
 *
 * Prebuilt OOM degradation handlers for all daemon services. Each service
 * registers its degradation callbacks via daemon_degradation_register_*()
 * at startup.
 *
 * Degradation policy mapping:
 *   WARNING  -> shrink cache 50%, lower log level, evict LRU
 *   HIGH     -> async IO downgraded to sync, smaller batch size
 *   CRITICAL -> reject new connections, suspend non-critical features
 *
 * @see oom_handler.h  OOM tiered response framework
 * @see svc_cache.h    cache service interface
 * @see svc_logger.h   logging service interface
 */

#ifndef DAEMON_DEGRADATION_H
#define DAEMON_DEGRADATION_H

#include "airy_rt.h"
#include "oom_handler.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Degradation context types
 * ============================================================================ */

/**
 * @brief Cache degradation context.
 *
 * Saves the original cache capacity so it can be restored on recovery.
 */
typedef struct {
    void *cache_handle;
    size_t original_capacity;
    size_t reduced_capacity;
} degrade_cache_ctx_t;

/**
 * @brief Log degradation context.
 *
 * Saves the original log level so it can be restored on recovery.
 */
typedef struct {
    int original_log_level;
    int degraded_log_level;
} degrade_log_ctx_t;

/**
 * @brief Batch degradation context.
 *
 * Saves the original batch size so it can be restored on recovery.
 */
typedef struct {
    size_t *batch_size_ptr;
    size_t original_batch_size;
    size_t reduced_batch_size;
} degrade_batch_ctx_t;

/**
 * @brief Connection-control degradation context.
 *
 * Controls whether new connections are accepted.
 */
typedef struct {
    bool *reject_new_flag;
} degrade_conn_ctx_t;

/* ============================================================================
 * Prebuilt degradation handler registration
 * ============================================================================ */

/**
 * @brief Register a cache-capacity degradation handler.
 *
 * When the watermark reaches WATERMARK_WARNING, halves the cache capacity.
 * When the watermark recedes, restores the original capacity.
 *
 * @param cache_handle Cache handle (svc_cache_t * or compatible type)
 * @param original_capacity Original cache capacity
 * @return Degradation handler pointer (statically allocated; caller does not
 *         own it), NULL on failure
 */
degradation_handler_t *daemon_degradation_register_cache(void *cache_handle,
                                                         size_t original_capacity);

/**
 * @brief Register a log-level degradation handler.
 *
 * When the watermark reaches WATERMARK_WARNING, raises the log level to
 * ERROR. When the watermark recedes, restores the original level.
 *
 * @param original_log_level Original log level
 * @return Degradation handler pointer (statically allocated), NULL on failure
 */
degradation_handler_t *daemon_degradation_register_log_level(int original_log_level);

/**
 * @brief Register a batch-size degradation handler.
 *
 * When the watermark reaches WATERMARK_HIGH, halves the batch size.
 * When the watermark recedes, restores the original size.
 *
 * @param batch_size_ptr Pointer to the current batch size (runtime-mutable)
 * @param original_batch_size Original batch size
 * @return Degradation handler pointer (statically allocated), NULL on failure
 */
degradation_handler_t *daemon_degradation_register_batch(size_t *batch_size_ptr,
                                                         size_t original_batch_size);

/**
 * @brief Register a new-connection rejection degradation handler.
 *
 * When the watermark reaches WATERMARK_CRITICAL, sets the reject-new-flag.
 * When the watermark recedes, clears the flag.
 *
 * @param reject_flag Pointer to the reject flag (runtime-mutable)
 * @return Degradation handler pointer (statically allocated), NULL on failure
 */
degradation_handler_t *daemon_degradation_register_reject_conn(bool *reject_flag);

/**
 * @brief Register a custom degradation handler.
 *
 * Lets a daemon service register custom degrade/restore callbacks.
 *
 * @param feature_name Feature name (for logging)
 * @param trigger_level Watermark level that triggers degradation
 * @param on_degrade Degradation callback
 * @param on_restore Restore callback
 * @param context User context
 * @return Degradation handler pointer (statically allocated), NULL on failure
 */
degradation_handler_t *daemon_degradation_register_custom(
    const char *feature_name, watermark_level_t trigger_level,
    int (*on_degrade)(degradation_handler_t *, watermark_level_t, watermark_level_t),
    int (*on_restore)(degradation_handler_t *, watermark_level_t, watermark_level_t),
    void *context);

/**
 * @brief Unregister all daemon degradation handlers.
 *
 * Call at daemon service shutdown to clean up all registered handlers.
 */
void daemon_degradation_unregister_all(void);

#ifdef __cplusplus
}
#endif

#endif /* DAEMON_DEGRADATION_H */