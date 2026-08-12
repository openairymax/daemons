/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_heapstore_bootstrap.h
 * @brief Unified heapstore runtime data-store bootstrap for daemons.
 *
 * heapstore is the agentrt runtime data store (KER-05~07: syscall
 * session/trace persistence, linked by airy_core). This bootstrap gives
 * the daemon layer a unified init entry point:
 *   - daemon_heapstore_init(): in main() after airy_log_init(), before
 *     socket/service creation, initializes heapstore (idempotent)
 *   - daemon_heapstore_cleanup(): call before main() exits
 *   - daemon_heapstore_log(): service access-log write (used by gateway
 *     forwarding chains)
 *
 * Same pattern as daemon_cupolas_init: init failure logs FATAL but does
 * not abort; service layers degrade on unavailable storage (non-fatal,
 * keeping the daemon runnable).
 */

#ifndef AIRY_RT_DAEMON_HEAPSTORE_BOOTSTRAP_H
#define AIRY_RT_DAEMON_HEAPSTORE_BOOTSTRAP_H

#include "error.h" /* airy_err_t */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the heapstore runtime data store (unified bootstrap).
 *
 * Call in daemon main() after airy_log_init() (alongside
 * daemon_cupolas_init). Store root is $AIRY_HOME/data/agentrt/heapstore.
 *
 * @param daemon_name Daemon name (e.g. "gateway_d"), used in logs
 * @return AIRY_SUCCESS on success; error code on failure (FATAL logged,
 *         service may degrade)
 */
airy_err_t daemon_heapstore_init(const char *daemon_name);

/**
 * @brief Clean up the heapstore runtime data store.
 * Idempotent: repeated calls are safe.
 */
void daemon_heapstore_cleanup(void);

/**
 * @brief Write a service access log (called by gateway forwarding chains
 *        and other daemons).
 *
 * Silently ignored when heapstore is unavailable (uninitialized/write
 * failure); returns non-zero.
 *
 * @param module Module name (e.g. "gateway_d")
 * @param level  Log level (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR)
 * @param msg    Log message
 * @param trace_id Trace ID (may be NULL)
 * @return 0 on success; non-zero if store unavailable/failed
 */
int daemon_heapstore_log(const char *module, int level, const char *msg, const char *trace_id);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_HEAPSTORE_BOOTSTRAP_H */
