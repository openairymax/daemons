/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_oom.h
 * @brief Daemon OOM degradation callback registration helper.
 *
 * P1.22: standardized OOM degradation callbacks for every daemon.
 * WARNING-level drops caches, CRITICAL-level rejects requests.
 */

#ifndef AIRY_RT_DAEMON_OOM_H
#define AIRY_RT_DAEMON_OOM_H

#include "oom_handler.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Daemon OOM degradation configuration. */
typedef struct {
    const char *daemon_name;
    bool drop_cache_on_warning;
    bool reject_requests_on_critical;
    void *user_context;
} daemon_oom_config_t;

/**
 * @brief Register the standard OOM degradation callbacks for a daemon.
 *
 * Registers WARNING and CRITICAL callbacks:
 *   - WARNING:  drop caches, lower log level
 *   - CRITICAL: reject new requests, suspend non-critical features
 *
 * @param config Configuration
 * @return 0 on success, non-zero on failure
 */
int daemon_oom_register(const daemon_oom_config_t *config);

/**
 * @brief Unregister a daemon's OOM degradation callbacks.
 *
 * @param daemon_name Daemon name
 */
void daemon_oom_unregister(const char *daemon_name);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_OOM_H */
