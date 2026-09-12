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

#include <cjson/cJSON.h>
#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

/** Default call timeout, mirroring DAEMON_RPC_DEFAULT_TIMEOUT_MS on the
 * socket path (daemon_rpc_client.c) — both transports must not drift. */
#define DAEMON_L2_RPC_DEFAULT_TIMEOUT_MS 30000u

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

int daemon_l2_channel_for_socket(const char *socket_path, char *channel, size_t channel_size)
{
    if (!socket_path || !channel || channel_size == 0)
        return AIRY_EINVAL;

    const char *base = strrchr(socket_path, '/');
#ifdef _WIN32
    const char *wbase = strrchr(socket_path, '\\');
    if (wbase && (!base || wbase > base))
        base = wbase;
#endif
    base = base ? base + 1 : socket_path;

    /* "<ns>.sock" -> "<ns>.rpc": the ".sock" suffix is the naming
     * discriminator — anything else (TCP "host:port", foreign UDS names)
     * stays on the socket path. */
    size_t base_len = strlen(base);
    const size_t suffix_len = 5; /* strlen(".sock") */
    if (base_len <= suffix_len || strcmp(base + base_len - suffix_len, ".sock") != 0)
        return AIRY_EINVAL;

    size_t ns_len = base_len - suffix_len;
    if (ns_len + sizeof(".rpc") > channel_size)
        return AIRY_EMSGSIZE;

    /* Gate on the transport switch (upper-case ns, daemon_l1_server.h):
     * handing out a channel nobody mounts would silently move traffic
     * onto a dead path. */
    char ns_upper[64];
    if (ns_len >= sizeof(ns_upper))
        return AIRY_EMSGSIZE;
    for (size_t i = 0; i < ns_len; i++)
        ns_upper[i] = (char)toupper((unsigned char)base[i]);
    ns_upper[ns_len] = '\0';
    if (!daemon_l1_transport_enabled(ns_upper))
        return AIRY_ERR_NOT_FOUND;

    AIRY_MEMCPY(channel, base, ns_len);
    AIRY_MEMCPY(channel + ns_len, ".rpc", sizeof(".rpc"));
    return 0;
}

/**
 * Serializes the JSON-RPC 2.0 request exactly as rpc_connect_send does on
 * the socket path (id=1; params embedded when valid JSON, stringified
 * otherwise, {} when empty) so the daemon sees identical requests on both
 * transports. Returns the PrintUnformatted buffer (cJSON default
 * allocator, AIRY_FREE-compatible) or NULL on OOM.
 */
static char *daemon_l2_build_request(const char *method, const char *params_json)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", method);
    if (params_json && params_json[0] != '\0') {
        cJSON *params = cJSON_Parse(params_json);
        if (params) {
            cJSON_AddItemToObject(root, "params", params);
        } else {
            cJSON_AddStringToObject(root, "params", params_json);
        }
    } else {
        cJSON_AddObjectToObject(root, "params");
    }
    cJSON_AddNumberToObject(root, "id", 1);
    char *request_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return request_str;
}

/**
 * L2 transaction core: build -> envelope -> connect -> call -> decode. On
 * success *out_resp_json is the daemon's complete reply payload as a
 * NUL-terminated string — nothing parsed, nothing folded, error responses
 * included verbatim. On any transport failure *out_resp_json stays NULL and
 * the transport code propagates so callers can fall back to the socket path
 * (blueprint 8.3.3 grey rollout).
 */
static int daemon_l2_rpc_transact(const char *channel, const char *method,
                                  const char *params_json, uint32_t timeout_ms,
                                  char **out_resp_json)
{
    if (!out_resp_json)
        return AIRY_ERR_INVALID_PARAM;
    *out_resp_json = NULL;
    if (!channel || !method)
        return AIRY_ERR_INVALID_PARAM;

    char *request_str = daemon_l2_build_request(method, params_json);
    if (!request_str)
        return AIRY_ERR_OUT_OF_MEMORY;
    size_t req_len = strlen(request_str);

    int rc = AIRY_ERR_GENERIC_FAIL;
    uint8_t *req_env = NULL;
    uint8_t *resp_env = NULL;
    airy_ipc_channel_t *client = NULL;

    if (req_len > DAEMON_L2_MAX_PAYLOAD) {
        rc = AIRY_EMSGSIZE;
        goto out;
    }

    /* 512 KiB response bound: heap, never stack. */
    req_env = AIRY_MALLOC((size_t)AIRY_IPC_HDR_SIZE + req_len);
    resp_env = AIRY_MALLOC((size_t)AIRY_IPC_HDR_SIZE + DAEMON_L2_MAX_PAYLOAD);
    if (!req_env || !resp_env) {
        rc = AIRY_ERR_OUT_OF_MEMORY;
        goto out;
    }
    if (daemon_l2_envelope_encode(0, daemon_l2_self_task(), 0, request_str, req_len, req_env,
                                  (size_t)AIRY_IPC_HDR_SIZE + req_len) != 0) {
        rc = AIRY_EMSGSIZE;
        goto out;
    }

    airy_err_t cerr = airy_ipc_connect(channel, &client);
    if (cerr != AIRY_SUCCESS) {
        /* Transport-layer code (e.g. NOT_FOUND: no bridge mounted)
         * propagates so callers can fall back to the socket path. */
        rc = (int)cerr;
        goto out;
    }

    size_t resp_size = (size_t)AIRY_IPC_HDR_SIZE + DAEMON_L2_MAX_PAYLOAD;
    airy_kernel_ipc_message_t msg = {.code = 1,
                                     .data = req_env,
                                     .size = (size_t)AIRY_IPC_HDR_SIZE + req_len,
                                     .fd = -1,
                                     .msg_id = 1};
    airy_err_t err = airy_ipc_call(client, &msg, resp_env, &resp_size, timeout_ms);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_WARN("daemon_l2_rpc: call failed (method=%s, err=%d)", method, (int)err);
        /* CANCELED = dropped/undispatched transaction (L2 §2.3): the
         * socket path folds transport loss into GENERIC_FAIL too. */
        rc = (err == AIRY_ERR_CANCELED) ? AIRY_ERR_GENERIC_FAIL : (int)err;
        goto out;
    }

    const void *resp_payload = NULL;
    size_t resp_len = 0;
    uint64_t trace_id = 0;
    uint64_t src_task = 0;
    if (daemon_l2_envelope_decode(resp_env, resp_size, &resp_payload, &resp_len, &trace_id,
                                  &src_task) != 0) {
        SVC_LOG_ERROR("daemon_l2_rpc: malformed reply envelope (method=%s)", method);
        rc = AIRY_ERR_GENERIC_FAIL;
        goto out;
    }
    if (resp_len == 0 || !resp_payload) {
        /* A JSON-RPC reply is never empty: malformed exchange, not a
         * transport loss — fold like daemon_rpc_call_cancelable. */
        SVC_LOG_ERROR("daemon_l2_rpc: empty reply payload (method=%s)", method);
        rc = AIRY_ERR_GENERIC_FAIL;
        goto out;
    }

    /* Payload view has no NUL guarantee; JSON consumers need one. */
    char *resp_json = AIRY_MALLOC(resp_len + 1);
    if (!resp_json) {
        rc = AIRY_ERR_OUT_OF_MEMORY;
        goto out;
    }
    AIRY_MEMCPY(resp_json, resp_payload, resp_len);
    resp_json[resp_len] = '\0';
    *out_resp_json = resp_json;
    rc = AIRY_SUCCESS;

out:
    if (client)
        airy_ipc_close(client);
    AIRY_FREE(resp_env);
    AIRY_FREE(req_env);
    AIRY_FREE(request_str);
    return rc;
}

int daemon_l2_rpc_call_resp(const char *channel, const char *method, const char *params_json,
                            uint32_t timeout_ms, char **out_resp_json)
{
    if (timeout_ms == 0)
        timeout_ms = DAEMON_L2_RPC_DEFAULT_TIMEOUT_MS;
    return daemon_l2_rpc_transact(channel, method, params_json, timeout_ms, out_resp_json);
}

int daemon_l2_rpc_call(const char *channel, const char *method, const char *params_json,
                       char **out_result_json, uint32_t timeout_ms)
{
    if (!out_result_json)
        return AIRY_ERR_INVALID_PARAM;
    *out_result_json = NULL;
    if (timeout_ms == 0)
        timeout_ms = DAEMON_L2_RPC_DEFAULT_TIMEOUT_MS;

    char *resp_json = NULL;
    int rc = daemon_l2_rpc_transact(channel, method, params_json, timeout_ms, &resp_json);
    if (rc != AIRY_SUCCESS)
        return rc;

    cJSON *resp = cJSON_Parse(resp_json);
    AIRY_FREE(resp_json);
    if (!resp) {
        SVC_LOG_ERROR("daemon_l2_rpc_call: response parse failed (method=%s)", method);
        return AIRY_ERR_GENERIC_FAIL;
    }

    /* Fold rules mirror daemon_rpc_call_cancelable (daemon_rpc_client.c). */
    cJSON *err_obj = cJSON_GetObjectItem(resp, "error");
    if (err_obj) {
        cJSON *err_msg = cJSON_GetObjectItem(err_obj, "message");
        const char *emsg =
            (err_msg && cJSON_IsString(err_msg)) ? err_msg->valuestring : "unknown";
        cJSON *err_code = cJSON_GetObjectItem(err_obj, "code");
        int code = (err_code && cJSON_IsNumber(err_code)) ? err_code->valueint : -32000;
        SVC_LOG_WARN("daemon_l2_rpc_call: daemon returned error (method=%s, code=%d, msg=%s)",
                     method, code, emsg);
        cJSON_Delete(resp);
        return AIRY_ERR_GENERIC_FAIL;
    }

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (!result) {
        SVC_LOG_ERROR("daemon_l2_rpc_call: missing result field (method=%s)", method);
        cJSON_Delete(resp);
        return AIRY_ERR_GENERIC_FAIL;
    }

    char *result_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(resp);
    if (!result_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    *out_result_json = AIRY_STRDUP(result_str);
    AIRY_FREE(result_str);
    return *out_result_json ? AIRY_SUCCESS : AIRY_ERR_OUT_OF_MEMORY;
}
