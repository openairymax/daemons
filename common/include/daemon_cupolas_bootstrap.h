/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_cupolas_bootstrap.h
 * @brief P3.14 (ACC-DT15): unified cupolas security dome bootstrap.
 *
 * Provides a single init/cleanup interface for the cupolas security dome
 * across all daemons, avoiding duplicated code in the 12 main.c files.
 *
 * Call contract:
 *   - call daemon_cupolas_init() in main() after airy_log_init(), before
 *     socket/service creation
 *   - call daemon_cupolas_cleanup() before main() exits
 *   - on init failure resources are already released; no cleanup needed
 *
 * Security semantics:
 *   - cupolas_init uses the default config (NULL config_path), enabling the
 *     permission_engine + sanitizer + audit_logger submodules
 *   - on failure a FATAL log is emitted but the process is not aborted (the
 *     daemon may run degraded; each service layer's fail-closed logic blocks
 *     dangerous operations; this avoids a system that cannot start merely
 *     because the security module failed to initialize)
 *   - repeated init calls are idempotent (guarded inside cupolas_init)
 */

#ifndef AIRY_RT_DAEMON_CUPOLAS_BOOTSTRAP_H
#define AIRY_RT_DAEMON_CUPOLAS_BOOTSTRAP_H

#include "error.h" /* airy_err_t */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the cupolas security dome (unified bootstrap).
 *
 * Call in daemon main() after airy_log_init(). Initializes the
 * permission_engine, sanitizer and audit_logger submodules.
 *
 * @param daemon_name Daemon name (e.g. "tool_d", "llm_d"), used for audit logs
 * @return AIRY_SUCCESS on success; error code on failure (FATAL already logged)
 *
 * @ownership daemon_name: BORROW (caller keeps ownership)
 */
airy_err_t daemon_cupolas_init(const char *daemon_name);

/**
 * @brief Clean up the cupolas security dome.
 *
 * Call before daemon main() exits. Flushes audit logs and releases
 * resources. Idempotent: repeated calls are safe.
 */
void daemon_cupolas_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_CUPOLAS_BOOTSTRAP_H */
