/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_llm_ops_bootstrap.h
 * @brief LLM service ops-table bootstrap (ARC-02/ARC-04).
 *
 * ARC-02/ARC-04: atoms code (cognition engine, orchestrator, language
 * gateway) must not link against daemons symbols. It dispatches through
 * airy_llm_ops_t instead; the daemon layer installs the concrete
 * implementation (airy_llm_service: completion, streaming completion and
 * response release) at startup:
 *   - daemon_llm_ops_init(): in main() after airy_log_init(), alongside
 *     daemon_cupolas_init()/daemon_heapstore_init() (idempotent)
 *   - daemon_llm_ops_cleanup(): call before main() exits
 *
 * After cleanup the table is detached and atoms call sites degrade
 * gracefully (BAN-319). Init failure is logged but never fatal: a daemon
 * without injected LLM ops stays runnable, only the atoms LLM path
 * degrades.
 */

#ifndef AIRY_RT_DAEMON_LLM_OPS_BOOTSTRAP_H
#define AIRY_RT_DAEMON_LLM_OPS_BOOTSTRAP_H

#include "error.h" /* airy_err_t */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the LLM service ops implementation.
 *
 * Call in daemon main() after airy_log_init(). Idempotent: repeated calls
 * are safe and only the first installs the table.
 *
 * @param daemon_name Daemon name (e.g. "llm_d"), used in logs
 * @return AIRY_SUCCESS on success; error code on invalid argument
 */
airy_err_t daemon_llm_ops_init(const char *daemon_name);

/**
 * @brief Detach the LLM service ops implementation.
 * Idempotent: repeated calls are safe.
 */
void daemon_llm_ops_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_LLM_OPS_BOOTSTRAP_H */
