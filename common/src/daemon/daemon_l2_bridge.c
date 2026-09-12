// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file daemon_l2_bridge.c
 * @brief L2 service envelope bridge on the corekern L1 transport
 *        (blueprint 8.3.2).
 *
 * Sister module of daemon_l1_server.c (same static library, own TU): both
 * are the daemon-side mount family on the corekern L1 binder. A separate
 * target would add a link edge for no isolation benefit, and the L2 bridge
 * additionally needs the L1 channel handle in its callback to reply — the
 * reason daemon_l1_handler_fn stays channel-less while this module owns the
 * corekern types directly.
 *
 * Wire contract (L2 standard §1.3, [SC] SSoT airymax/ipc.h): one L1
 * transaction carries a bare struct airy_ipc_msg_hdr (128B Layout C v4)
 * followed by the JSON-RPC payload. magic is the discriminator; the
 * JSON-RPC body is self-describing, so encode() fills AIRY_IPC_OP_SEND and
 * decode() does not validate the opcode. Timestamps are monotonic via
 * airy_time_ns() (8.2.3 SSoT); the payload CRC32 is the IEEE 802.3
 * computation exposed by the [SC] contract layer (airy_task_desc_crc32 —
 * bit-identical to the IPC-domain helper, but public and all-platform).
 *
 * Drop policy (L2 §2.3): malformed envelopes are logged (WARN) and dropped
 * with no ERROR reply — the binder surfaces AIRY_ERR_CANCELED to the
 * sender. Dispatch responses exceeding DAEMON_L2_MAX_PAYLOAD are rejected
 * before any wire-length arithmetic (size_t wrap safety).
 *
 * Concurrency model: identical to daemon_l1_server.c — synchronous handler
 * on the sender's thread, stop() drains via detached flag + inflight
 * counter, so a stop racing concurrent senders never frees the dispatch
 * context while a transaction is inside it.
 */

#include "daemon_l1_server.h"

#include "ipc.h"
#include "platform_misc.h"
#include "svc_logger.h"

#include <airymax/ipc.h>       /* [SC] SSoT: AIRY_IPC_MAGIC (P0-05 convergence) */
#include <airymax/task_desc.h> /* [SC] CRC-32 IEEE 802.3 (all-platform public form) */

#include "airy_memory.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

struct daemon_l2_bridge {
    daemon_l2_dispatch_fn dispatch;
    void *userdata;
    airy_ipc_channel_t *channel;
    atomic_bool detached; /**< stop() set: refuse and fail new transactions */
    atomic_int inflight;  /**< transactions currently inside dispatch */
};

/** Envelope src_task for daemon-originated replies (process identifier). */
static uint64_t daemon_l2_self_task(void)
{
#ifdef _WIN32
    return (uint64_t)_getpid();
#else
    return (uint64_t)getpid();
#endif
}

/**
 * corekern callback bridge: invoked by the binder on the sender's thread.
 * Decode (L2 §2.3 drop) -> dispatch -> encode -> reply. Every failure path
 * decrements the inflight counter before returning so stop() can free.
 */
static airy_err_t daemon_l2_bridge_cb(airy_ipc_channel_t *channel,
                                      const airy_kernel_ipc_message_t *msg, void *userdata)
{
    daemon_l2_bridge_t *bridge = (daemon_l2_bridge_t *)userdata;

    if (!bridge || atomic_load_explicit(&bridge->detached, memory_order_acquire)) {
        return AIRY_ERR_CANCELED;
    }

    atomic_fetch_add_explicit(&bridge->inflight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&bridge->detached, memory_order_acquire)) {
        /* stop() raced between the first check and the counter; bail out
         * before touching dispatch/userdata so stop may free them. */
        atomic_fetch_sub_explicit(&bridge->inflight, 1, memory_order_acq_rel);
        return AIRY_ERR_CANCELED;
    }

    const void *req_payload = NULL;
    size_t req_len = 0;
    uint64_t trace_id = 0;
    uint64_t src_task = 0;
    int drc = daemon_l2_envelope_decode(msg->data, msg->size, &req_payload, &req_len,
                                        &trace_id, &src_task);
    if (drc != 0) {
        /* L2 §2.3: malformed envelope — log, drop, send no reply. */
        SVC_LOG_WARN("daemon_l2_bridge: dropped malformed envelope (%d) len=%zu", drc,
                     msg->size);
        atomic_fetch_sub_explicit(&bridge->inflight, 1, memory_order_acq_rel);
        return AIRY_ERR_CANCELED;
    }

    char *resp_json = NULL;
    size_t resp_len = 0;
    int rc = bridge->dispatch((const char *)req_payload, req_len, &resp_json, &resp_len,
                              bridge->userdata);
    if (rc != 0) {
        SVC_LOG_ERROR("daemon_l2_bridge: dispatch failed (%d) trace_id=%llu", rc,
                      (unsigned long long)trace_id);
        atomic_fetch_sub_explicit(&bridge->inflight, 1, memory_order_acq_rel);
        return AIRY_ERR_CANCELED;
    }

    if (resp_len > DAEMON_L2_MAX_PAYLOAD) {
        /* Reject before wire_len arithmetic: keeps 128 + resp_len free of
         * size_t wrap regardless of what dispatch produced. */
        SVC_LOG_ERROR("daemon_l2_bridge: response too large (%zu) trace_id=%llu", resp_len,
                      (unsigned long long)trace_id);
        AIRY_FREE(resp_json);
        atomic_fetch_sub_explicit(&bridge->inflight, 1, memory_order_acq_rel);
        return AIRY_ERR_CANCELED;
    }

    size_t wire_len = (size_t)AIRY_IPC_HDR_SIZE + resp_len;
    void *wire = AIRY_MALLOC(wire_len);
    if (!wire) {
        SVC_LOG_ERROR("daemon_l2_bridge: envelope alloc failed (%zu)", wire_len);
        AIRY_FREE(resp_json);
        atomic_fetch_sub_explicit(&bridge->inflight, 1, memory_order_acq_rel);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    int erc = daemon_l2_envelope_encode(trace_id, daemon_l2_self_task(), src_task,
                                        resp_json, resp_len, wire, wire_len);
    if (erc != 0) {
        SVC_LOG_ERROR("daemon_l2_bridge: response encode failed (%d) trace_id=%llu", erc,
                      (unsigned long long)trace_id);
        AIRY_FREE(wire);
        AIRY_FREE(resp_json);
        atomic_fetch_sub_explicit(&bridge->inflight, 1, memory_order_acq_rel);
        return AIRY_ERR_CANCELED;
    }

    airy_kernel_ipc_message_t reply = {.code = msg->code,
                                       .data = wire,
                                       .size = wire_len,
                                       .fd = -1,
                                       .msg_id = msg->msg_id};
    airy_err_t err = airy_ipc_reply(channel, &reply);

    AIRY_FREE(wire);
    AIRY_FREE(resp_json);
    atomic_fetch_sub_explicit(&bridge->inflight, 1, memory_order_acq_rel);

    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("daemon_l2_bridge: reply failed (%d) trace_id=%llu", err,
                      (unsigned long long)trace_id);
    }
    return err;
}

daemon_l2_bridge_t *daemon_l2_bridge_start(const char *channel_name,
                                           daemon_l2_dispatch_fn dispatch, void *userdata)
{
    if (!channel_name || *channel_name == '\0' || !dispatch) {
        SVC_LOG_ERROR("daemon_l2_bridge_start: null/empty channel_name or dispatch");
        return NULL;
    }
    /* No name-length pre-check: corekern create_channel is the single
     * validator (rejects over-long names with EINVAL; proven by the 8.3.1
     * suite) — duplicating the bound here would fork the constant. */

    /* Idempotent (CAS swap inside corekern); daemons that already ran
     * airy_init() reach here initialized. */
    airy_err_t irc = airy_ipc_init();
    if (irc != AIRY_SUCCESS) {
        SVC_LOG_ERROR("daemon_l2_bridge_start: airy_ipc_init failed (%d)", irc);
        return NULL;
    }

    daemon_l2_bridge_t *bridge = (daemon_l2_bridge_t *)AIRY_CALLOC(1, sizeof(*bridge));
    if (!bridge) {
        SVC_LOG_ERROR("daemon_l2_bridge_start: alloc failed");
        return NULL;
    }

    bridge->dispatch = dispatch;
    bridge->userdata = userdata;
    atomic_store_explicit(&bridge->detached, false, memory_order_relaxed);
    atomic_store_explicit(&bridge->inflight, 0, memory_order_relaxed);

    airy_err_t rc = airy_ipc_create_channel(channel_name, daemon_l2_bridge_cb, bridge,
                                            &bridge->channel);
    if (rc != AIRY_SUCCESS) {
        SVC_LOG_ERROR("daemon_l2_bridge_start: create_channel failed (%d) name=%s", rc,
                      channel_name);
        AIRY_FREE(bridge);
        return NULL;
    }

    SVC_LOG_INFO("daemon_l2_bridge: mounted L2 channel '%s' on corekern L1", channel_name);
    return bridge;
}

void daemon_l2_bridge_stop(daemon_l2_bridge_t *bridge)
{
    if (!bridge) {
        return;
    }

    /* Detach first: new transactions fail fast inside the bridge. */
    atomic_store_explicit(&bridge->detached, true, memory_order_release);

    if (bridge->channel) {
        airy_ipc_close(bridge->channel);
        bridge->channel = NULL;
    }

    /* Drain: wait for transactions already inside dispatch. The binder
     * invokes the callback without locks, so every in-flight call reaches
     * its counter decrement without further coordination. */
    while (atomic_load_explicit(&bridge->inflight, memory_order_acquire) > 0) {
        airy_sleep_ms(1);
    }

    SVC_LOG_INFO("daemon_l2_bridge: unmounted L2 channel");
    AIRY_FREE(bridge);
}

int daemon_l2_envelope_encode(uint64_t trace_id, uint64_t src_task, uint64_t dst_task,
                              const void *payload, size_t payload_len, void *out_buf,
                              size_t out_size)
{
    if (!out_buf || (!payload && payload_len > 0)) {
        return AIRY_EINVAL;
    }
    if (payload_len > DAEMON_L2_MAX_PAYLOAD ||
        out_size < (size_t)AIRY_IPC_HDR_SIZE + payload_len) {
        return AIRY_EMSGSIZE;
    }

    /* memset zeroes flags and the 72 reserved bytes by construction — the
     * decode-side C-S10 validation passes without field-by-field zeroing. */
    struct airy_ipc_msg_hdr hdr;
    AIRY_MEMSET(&hdr, 0, sizeof(hdr));
    hdr.magic = AIRY_IPC_MAGIC;
    hdr.opcode = AIRY_IPC_OP_SEND;
    hdr.trace_id = trace_id;
    hdr.timestamp_ns = airy_time_ns();
    hdr.src_task = src_task;
    hdr.dst_task = dst_task;
    hdr.payload_len = (__u32)payload_len;
    hdr.crc32 = payload_len > 0 ? airy_task_desc_crc32(payload, payload_len) : 0;

    /* memcpy both header and payload: out_buf carries no alignment
     * guarantee and the [SC] struct is aligned(64). */
    AIRY_MEMCPY(out_buf, &hdr, sizeof(hdr));
    if (payload_len > 0) {
        AIRY_MEMCPY((uint8_t *)out_buf + AIRY_IPC_HDR_SIZE, payload, payload_len);
    }
    return 0;
}

int daemon_l2_envelope_decode(const void *buf, size_t buf_size, const void **out_payload,
                              size_t *out_payload_len, uint64_t *out_trace_id,
                              uint64_t *out_src_task)
{
    if (!buf || !out_payload || !out_payload_len || !out_trace_id || !out_src_task) {
        return AIRY_EINVAL;
    }
    if (buf_size < (size_t)AIRY_IPC_HDR_SIZE) {
        return AIRY_EMSGSIZE;
    }

    struct airy_ipc_msg_hdr hdr;
    AIRY_MEMCPY(&hdr, buf, sizeof(hdr));

    if (hdr.magic != AIRY_IPC_MAGIC || (hdr.flags & AIRY_IPC_FLAG_RESERVED) != 0) {
        return AIRY_ERR_PROTOCOL;
    }
    if (hdr.payload_len > DAEMON_L2_MAX_PAYLOAD ||
        buf_size != (size_t)AIRY_IPC_HDR_SIZE + hdr.payload_len) {
        return AIRY_EMSGSIZE;
    }
    for (size_t i = 0; i < sizeof(hdr.reserved); i++) {
        if (hdr.reserved[i] != 0) {
            return AIRY_ERR_PROTOCOL;
        }
    }
    if (hdr.payload_len > 0 &&
        hdr.crc32 != airy_task_desc_crc32((const uint8_t *)buf + AIRY_IPC_HDR_SIZE,
                                          hdr.payload_len)) {
        return AIRY_ERR_CHECKSUM;
    }

    *out_payload = hdr.payload_len > 0 ? (const uint8_t *)buf + AIRY_IPC_HDR_SIZE : NULL;
    *out_payload_len = hdr.payload_len;
    *out_trace_id = hdr.trace_id;
    *out_src_task = hdr.src_task;
    return 0;
}
