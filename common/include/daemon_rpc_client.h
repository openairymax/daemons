/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_rpc_client.h
 * @brief Lightweight Unix-socket JSON-RPC client.
 *
 * Phase-3 executor consolidation refactor: a thin client for migrating
 * syscall_router.c in gateway_d from in-process implementation to daemon
 * IPC. Exposes only synchronous blocking calls; internally handles socket
 * connection, JSON-RPC 2.0 request construction, response parsing and
 * result extraction.
 *
 * Design goals:
 *   - Strictly aligned with the daemon-side JSON-RPC 2.0 over Unix socket
 *   - Self-contained, no libcurl dependency (unlike ipc_client.h)
 *   - Caller frees the returned result_json string via AIRY_FREE
 *
 */

#ifndef AIRY_RT_DAEMON_RPC_CLIENT_H
#define AIRY_RT_DAEMON_RPC_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "cancel_token.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Call a JSON-RPC method on a daemon (Unix socket, synchronous).
 *
 * @param socket_path  Daemon Unix socket path (non-NULL)
 * @param method       JSON-RPC method name (non-NULL, without namespace
 *                     prefix, e.g. "write")
 * @param params_json  Serialized params object (NULL = empty params)
 * @param out_result_json Serialized result-field string (caller AIRY_FREEs)
 * @param timeout_ms   Timeout in ms, 0 = default 30000ms
 * @return AIRY_SUCCESS on success; other codes on failure
 *
 * @note On failure *out_result_json is not set (stays NULL).
 *       Available on POSIX only; returns AIRY_ERR_NOT_SUPPORTED on Windows.
 */
int daemon_rpc_call(const char *socket_path, const char *method, const char *params_json,
                    char **out_result_json, uint32_t timeout_ms);

/**
 * @brief Cancelable daemon JSON-RPC call (improvement 1 "cancel down-probe").
 *
 * Same as daemon_rpc_call, but short-polls the cancel token while waiting
 * for the response (non-blocking select/poll, 200ms slices). On token hit,
 * first sends a cancel request to the same daemon (agent_d: agent.cancel
 * terminates the invoke session by request_id), then returns
 * AIRY_ERR_CANCELED. Used for DAG-node-level cancellation -> tool/agent
 * call-level cancellation down-probe.
 *
 * @param socket_path  Daemon Unix socket path (non-NULL)
 * @param method       JSON-RPC method name (e.g. "invoke")
 * @param params_json  Serialized params object (may be NULL)
 * @param out_result_json Serialized result JSON string (caller AIRY_FREEs)
 * @param timeout_ms   Timeout in ms, 0 = default 30000ms
 * @param cancel_token Cancel token (NULL = same as daemon_rpc_call)
 * @param cancel_method    Method name sent on cancel (e.g. "cancel")
 * @param cancel_params_json params JSON of the cancel request (e.g. {"request_id":...})
 * @return AIRY_SUCCESS on success; AIRY_ERR_CANCELED on cancellation (cancel
 *         request delivered); other codes on failure
 */
int daemon_rpc_call_cancelable(const char *socket_path, const char *method, const char *params_json,
                               char **out_result_json, uint32_t timeout_ms,
                               airy_cancel_token_t *cancel_token, const char *cancel_method,
                               const char *cancel_params_json);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_RPC_CLIENT_H */
