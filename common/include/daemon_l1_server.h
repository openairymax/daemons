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

/* ===== L2 service envelope bridge (blueprint 8.3.2) =====
 *
 * Wire format (L2 §1.3): a bare `struct airy_ipc_msg_hdr` ([SC] SSoT,
 * commons/include/airymax/ipc.h) followed by the JSON-RPC payload, carried
 * as the data of one corekern L1 transaction. magic is the discriminator;
 * the JSON-RPC body is self-describing, so decode() does not validate the
 * opcode (encode() always fills AIRY_IPC_OP_SEND) and the reply reuses the
 * request's L1 code.
 *
 * Drop policy (L2 §2.3): a malformed envelope (bad magic / reserved flag
 * bits / non-zero reserved bytes / CRC32 mismatch) is logged and dropped —
 * the bridge returns AIRY_ERR_CANCELED to the binder and sends no ERROR
 * reply, mirroring the C-S10 fail-silent contract.
 *
 * The bridge mounts like daemon_l1_server (binder-style synchronous handler
 * on the sender's thread, drain-on-stop) and adapts the L1 transaction to a
 * JSON-string dispatch callback. Same svc_common macro-isolation boundary:
 * no corekern headers leak through this header.
 */

/** Maximum JSON-RPC payload carried in one envelope (either direction). */
#define DAEMON_L2_MAX_PAYLOAD (512u * 1024u)

/** Opaque L2 envelope bridge handle. */
typedef struct daemon_l2_bridge daemon_l2_bridge_t;

/**
 * @brief L2 request dispatch callback (the daemon's JSON-RPC boundary).
 *
 * Executed on the sender's thread for every well-formed envelope.
 *
 * @param req_json  [in] request payload view (zero-copy, valid during the
 *                       call only; NULL when req_len == 0)
 * @param req_len   [in] request payload length in bytes (no NUL guaranteed)
 * @param resp_json [out] response string allocated by the implementation in
 *                        the AIRY_MALLOC domain (the bridge releases it with
 *                        AIRY_FREE); may be NULL only when *resp_len == 0
 * @param resp_len  [out] response length in bytes, excluding any NUL
 * @param userdata  [in] the pointer given at daemon_l2_bridge_start()
 * @return 0 on success; non-zero drops the transaction (the sender's
 *         airy_ipc_call surfaces AIRY_ERR_CANCELED)
 */
typedef int (*daemon_l2_dispatch_fn)(const char *req_json, size_t req_len,
                                     char **resp_json, size_t *resp_len, void *userdata);

/**
 * @brief Mount an L2 envelope bridge on a corekern L1 channel.
 *
 * Same mount semantics as daemon_l1_server_start(): idempotent corekern IPC
 * init, EEXIST on duplicate names (surfaced as NULL). No name-length
 * pre-check here — corekern create_channel is the single validator.
 *
 * @param channel_name [in] globally unique channel name (e.g. "sched.rpc")
 * @param dispatch     [in] JSON-RPC dispatch callback (never invoked after
 *                          stop returns)
 * @param userdata     [in] opaque context passed through to dispatch
 * @return handle on success, NULL on failure (bad args, IPC init failure,
 *         duplicate name)
 * @ownership caller frees via daemon_l2_bridge_stop()
 */
daemon_l2_bridge_t *daemon_l2_bridge_start(const char *channel_name,
                                           daemon_l2_dispatch_fn dispatch, void *userdata);

/**
 * @brief Unmount the L2 bridge and release all resources.
 *
 * Drain semantics identical to daemon_l1_server_stop(): detach, close the
 * channel, wait for in-flight dispatch calls, then free. NULL is a safe
 * no-op.
 *
 * @param bridge [in] handle from daemon_l2_bridge_start(), may be NULL
 */
void daemon_l2_bridge_stop(daemon_l2_bridge_t *bridge);

/**
 * @brief Serialize one L2 envelope into a caller-provided buffer.
 *
 * Fills magic, opcode = AIRY_IPC_OP_SEND, flags = 0, trace_id, a monotonic
 * timestamp (airy_time_ns, 8.2.3 SSoT), src/dst task ids, payload_len and
 * the payload CRC32 (IEEE 802.3); reserved bytes are zeroed by
 * construction. Both header and payload are memcpy'd — out_buf carries no
 * alignment guarantee and the [SC] struct is aligned(64).
 *
 * @param trace_id    [in] distributed trace id (propagated to the reply)
 * @param src_task    [in] source task identifier
 * @param dst_task    [in] destination task identifier
 * @param payload     [in] payload bytes; NULL valid only when
 *                         payload_len == 0
 * @param payload_len [in] payload length in bytes (<= DAEMON_L2_MAX_PAYLOAD)
 * @param out_buf     [in] output buffer (not NULL)
 * @param out_size    [in] output buffer capacity; must be
 *                         >= AIRY_IPC_HDR_SIZE + payload_len
 * @return 0 on success; AIRY_EINVAL on NULL out_buf/payload misuse;
 *         AIRY_EMSGSIZE on size violations
 */
int daemon_l2_envelope_encode(uint64_t trace_id, uint64_t src_task, uint64_t dst_task,
                              const void *payload, size_t payload_len, void *out_buf,
                              size_t out_size);

/**
 * @brief Validate an L2 envelope and expose its payload as a zero-copy view.
 *
 * Validation order: header presence, magic + reserved flag bits
 * (AIRY_ERR_PROTOCOL), payload_len cap + strict total length
 * (AIRY_EMSGSIZE), reserved bytes (AIRY_ERR_PROTOCOL), payload CRC32
 * (AIRY_ERR_CHECKSUM). The view aliases buf and stays valid as long as the
 * envelope bytes do.
 *
 * @param buf            [in] envelope bytes (header + payload)
 * @param buf_size       [in] exact envelope size; must equal
 *                            AIRY_IPC_HDR_SIZE + payload_len
 * @param out_payload    [out] payload view (NULL when the payload is empty)
 * @param out_payload_len [out] payload length in bytes (0 for empty)
 * @param out_trace_id   [out] envelope trace id
 * @param out_src_task   [out] envelope source task id
 * @return 0 on success; AIRY_EINVAL on NULL args; AIRY_EMSGSIZE on size
 *         violations; AIRY_ERR_PROTOCOL on magic/flags/reserved corruption;
 *         AIRY_ERR_CHECKSUM on CRC32 mismatch
 */
int daemon_l2_envelope_decode(const void *buf, size_t buf_size, const void **out_payload,
                              size_t *out_payload_len, uint64_t *out_trace_id,
                              uint64_t *out_src_task);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_L1_SERVER_H */
