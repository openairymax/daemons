/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_l1_server.h
 * @brief Daemon-side server mount for the corekern L1 IPC transport
 *        (WS-8 stage 3, blueprint 8.3.1).
 *
 * Mounts a daemon service endpoint on the corekern L1 channel registry,
 * replacing (under the transport switch) the airy_sock_create_unix_server
 * listen path. The mount is opt-in: unless the transport switch resolves
 * to "corekern", no channel is created and daemon behavior is identical
 * to the pre-mount baseline (badge stays 0).
 *
 * Concurrency contract (corekern binder semantics, mirroring Android
 * Binder synchronous transactions):
 * - The handler runs synchronously on the *sender's* thread. There is no
 *   receiver thread and no internal queue: backpressure is the transaction
 *   itself (the sender blocks for the duration of the handler).
 * - `data` is a zero-copy view valid only for the duration of the handler
 *   call. Handlers that must defer processing must deep-copy.
 * - daemon_l1_server_stop() drains: it detaches the handler first, closes
 *   the channel, then waits for in-flight handler calls to return before
 *   freeing, so a stop racing concurrent senders is use-after-free safe.
 *
 * The corekern headers are intentionally NOT included here: this header
 * is consumed by svc_common, which must not link or macro-inherit from
 * airy_core (AIRY_USE_SCHEDULER_THREAD_IMPL would flip thread
 * implementation across all of svc_common). The L1 types stay an
 * implementation detail of daemon_l1_server.c.
 *
 * @see ARCHITECTURAL_PRINCIPLES.md ARC-02 (atoms side, reverse direction)
 * @see 0.1.15 architecture plan, WS-8 stage 3 / 8.3.1
 */

#ifndef AIRY_RT_DAEMON_L1_SERVER_H
#define AIRY_RT_DAEMON_L1_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque L1 server mount handle. */
typedef struct daemon_l1_server daemon_l1_server_t;

/**
 * @brief L1 transaction handler.
 *
 * Executed on the sender's thread for every transaction delivered to this
 * channel. Return 0 on success; a negative value is propagated back to
 * the sender as the transaction error code (airy_ipc_call surfaces it
 * directly; airy_ipc_send returns it from the sender's call site).
 *
 * @param code    [in] message type code chosen by the L2 envelope layer
 * @param data    [in] payload bytes (zero-copy, valid during call only)
 * @param size    [in] payload size in bytes
 * @param msg_id  [in] sender-assigned transaction id
 * @param userdata [in] the pointer given at daemon_l1_server_start()
 * @return 0 on success, negative airy_err_t on failure
 */
typedef int (*daemon_l1_handler_fn)(uint32_t code, const void *data, size_t size,
                                    uint64_t msg_id, void *userdata);

/**
 * @brief Mount a server endpoint on the L1 channel registry.
 *
 * Idempotently initializes the corekern IPC subsystem, then registers the
 * channel name globally (duplicate names fail with NULL, mirroring
 * EEXIST). The returned handle must be released with
 * daemon_l1_server_stop().
 *
 * @param channel_name [in] globally unique channel name (e.g. "sched.rpc")
 * @param handler      [in] transaction handler (never invoked after stop
 *                          returns)
 * @param userdata     [in] opaque context passed through to the handler
 * @return handle on success, NULL on failure (bad args, IPC init failure,
 *         duplicate name)
 * @ownership caller frees via daemon_l1_server_stop()
 */
daemon_l1_server_t *daemon_l1_server_start(const char *channel_name,
                                           daemon_l1_handler_fn handler, void *userdata);

/**
 * @brief Unmount the server endpoint and release all resources.
 *
 * Drain semantics: detaches the handler, closes the channel (new sends
 * fail), then blocks until in-flight handler invocations have returned,
 * then frees the handle. NULL is a safe no-op.
 *
 * @param svc [in] handle from daemon_l1_server_start(), may be NULL
 */
void daemon_l1_server_stop(daemon_l1_server_t *svc);

/**
 * @brief Channel name this mount was started with (for diagnostics).
 *
 * @param svc [in] handle from daemon_l1_server_start(), may be NULL
 * @return channel name, or NULL if svc is NULL
 */
const char *daemon_l1_server_channel_name(const daemon_l1_server_t *svc);

/**
 * @brief Resolve the transport switch for a daemon namespace.
 *
 * Three-level resolution aligned with the gateway sock resolution
 * (gw_resolve_daemon_sock): per-namespace override first, then the global
 * switch, then the default. The enabled value is "corekern"; the default
 * (nothing set) and the value "jsonrpc" keep the L1 mount off, preserving
 * the pre-mount behavior bit-for-bit. Any other value logs a warning and
 * resolves to off (fail-closed).
 *
 * @param ns_upper [in] namespace segment in upper case, e.g. "SCHED"
 *                     resolves AIRY_SCHED_IPC_TRANSPORT
 * @return true only when the switch resolves to "corekern"
 */
bool daemon_l1_transport_enabled(const char *ns_upper);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_L1_SERVER_H */
