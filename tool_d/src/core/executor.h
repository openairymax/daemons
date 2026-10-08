/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file executor.h
 * @brief Tool-executor handle and lifecycle.
 */

#ifndef TOOL_EXECUTOR_H
#define TOOL_EXECUTOR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool_executor tool_executor_t;

typedef struct {
    /* Execution-pool worker count. <= 0 means "pool default" (env
     * AIRY_TOOL_EXEC_WORKERS, else 2); env still overrides a positive value.
     * The executor itself never serializes tools - concurrency ownership
     * belongs to the pool. */
    int max_workers;
    int timeout_sec;
    char *workbench_type;
} tool_executor_config_t;

tool_executor_t *tool_executor_create(const tool_executor_config_t *cfg);
tool_executor_t *tool_executor_create_ex(const tool_executor_config_t *ecfg);
void tool_executor_destroy(tool_executor_t *exec);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_EXECUTOR_H */
