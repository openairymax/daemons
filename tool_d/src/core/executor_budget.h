/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file executor_budget.h
 * @brief Tool-executor deadline and worker-budget queries (R1-a deadline
 *        single source of truth, executor-wide).
 */

#ifndef TOOL_EXECUTOR_BUDGET_H
#define TOOL_EXECUTOR_BUDGET_H

#include "tool_service_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool_executor tool_executor_t;

/**
 * @brief Effective default per-tool budget in seconds (executor-wide).
 * @param exec Executor
 * @return Configured timeout (fallback applied at create time, always > 0)
 *
 * Fallback half of executor_budget_ms() - see that function for the
 * per-call deadline (per-tool metadata wins over this default).
 *
 * @ownership exec: BORROW
 */
int executor_timeout_sec(const tool_executor_t *exec);

/**
 * @brief Effective execution budget in milliseconds for one tool call.
 * @param exec Executor
 * @param meta Tool metadata (may be NULL)
 * @return Budget in ms (always > 0)
 *
 * R1-a single source of the deadline: per-tool metadata wins, otherwise the
 * executor default. Both execution paths inside tool_executor_run (builtin
 * dispatch and the external execvp path) and the pool's wait budget derive
 * from this function, so a tool can no longer be given one deadline by the
 * executor and a different one by its waiter.
 *
 * @ownership exec: BORROW; meta: BORROW
 */
uint32_t executor_budget_ms(const tool_executor_t *exec, const tool_metadata_t *meta);

/**
 * @brief Configured execution-pool worker count (raw config value).
 * @param exec Executor
 * @return Configured max_workers; <= 0 when unset (caller applies its default)
 *
 * @ownership exec: BORROW
 */
int executor_max_workers(const tool_executor_t *exec);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_EXECUTOR_BUDGET_H */
