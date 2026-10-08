/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file executor_gate.h
 * @brief Interactive-approval gate binding and control for the executor.
 */

#ifndef TOOL_EXECUTOR_GATE_H
#define TOOL_EXECUTOR_GATE_H

#include "approval_gate.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool_executor tool_executor_t;

/**
 * @brief Inject the approval gate (ownership transfers to the executor).
 * @param exec Executor
 * @param gate Approval gate (NULL re-arms fail-closed refusal)
 *
 * Without a gate every tool execution is refused (fail-closed); service.c
 * injects the default gate right after creating the executor.
 *
 * @ownership exec: BORROW, gate: TRANSFER
 */
void exec_set_gate(tool_executor_t *exec, approval_gate_t *gate);

/**
 * @brief Extra pool wait budget caused by interactive approval (0 when off).
 * @param exec Executor
 * @return the approval timeout SSoT when interactive approval is on, else 0
 *
 * Keeps the pool decoupled from the approval domain: the timeout SSoT
 * lives behind the gate.
 *
 * @ownership exec: BORROW
 */
uint64_t exec_int_budget_ms(const tool_executor_t *exec);

/**
 * @brief List all pending approval requests (JSON array string).
 * @param exec Executor
 * @return JSON array string (AIRY_MALLOC, caller AIRY_FREEs), NULL on failure
 *
 * @ownership exec: BORROW; return: OWNER
 */
char *exec_int_pending(tool_executor_t *exec);

/**
 * @brief Resolve a pending approval request by request_id.
 * @param exec Executor
 * @param request_id Request ID
 * @param decision Decision: "allow" / "always" / "deny"
 * @return 0 on success; AIRY_ERR_NOT_FOUND not found; AIRY_ERR_INVALID_PARAM bad args
 *
 * @ownership exec: BORROW
 */
int exec_int_resolve(tool_executor_t *exec, const char *request_id,
                     const char *decision);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_EXECUTOR_GATE_H */
