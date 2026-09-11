// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file daemon_l1_server.c
 * @brief Daemon-side server mount on the corekern L1 IPC transport
 *        (WS-8 stage 3, blueprint 8.3.1).
 *
 * Built as its own static library (not into svc_common) with a PRIVATE
 * link on airy_core: airy_core publishes AIRY_USE_SCHEDULER_THREAD_IMPL
 * as a PUBLIC compile definition, and svc_common sources contain
 * airy_thread_create call sites whose implementation selection must not
 * flip when this module is merely present. The tiny-library boundary
 * keeps the macro blast radius inside this translation unit.
 *
 * Concurrency model: corekern L1 is a synchronous binder-style
 * transaction layer - the handler runs on the sender's thread. stop()
 * drains with a detached flag plus an in-flight counter, so a stop
 * racing concurrent senders never frees the handler context while a
 * transaction is inside it.
 */

#include "daemon_l1_server.h"

#include "ipc.h"
#include "platform_misc.h"
#include "svc_logger.h"

#include "airy_memory.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/** Channel-name capacity; must stay in sync with corekern MAX_CHANNEL_NAME
 *  (binder_internal.h, private). corekern validates and rejects longer
 *  names with EINVAL, which start() surfaces as NULL. */
#define DAEMON_L1_NAME_MAX 64

struct daemon_l1_server {
    char name[DAEMON_L1_NAME_MAX];
    daemon_l1_handler_fn handler;
    void *userdata;
    airy_ipc_channel_t *channel;
    atomic_bool detached; /**< stop() set: refuse and fail new transactions */
    atomic_int inflight;  /**< transactions currently inside handler */
};

/**
 * corekern callback bridge: invoked by the binder on the sender's thread.
 * The channel argument is the server-side channel; handlers receive only
 * the envelope fields, keeping this module the sole owner of corekern
 * types (see daemon_l1_server.h rationale).
 */
static airy_err_t daemon_l1_bridge_cb(airy_ipc_channel_t *channel,
                                      const airy_kernel_ipc_message_t *msg, void *userdata)
{
    (void)channel;
    daemon_l1_server_t *svc = (daemon_l1_server_t *)userdata;

    if (!svc || atomic_load_explicit(&svc->detached, memory_order_acquire)) {
        return AIRY_ERR_CANCELED;
    }

    atomic_fetch_add_explicit(&svc->inflight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&svc->detached, memory_order_acquire)) {
        /* stop() raced between the first check and the counter; bail out
         * before touching handler/userdata so stop may free them. */
        atomic_fetch_sub_explicit(&svc->inflight, 1, memory_order_acq_rel);
        return AIRY_ERR_CANCELED;
    }

    int rc = svc->handler(msg->code, msg->data, msg->size, msg->msg_id, svc->userdata);

    atomic_fetch_sub_explicit(&svc->inflight, 1, memory_order_acq_rel);
    return (airy_err_t)rc;
}

daemon_l1_server_t *daemon_l1_server_start(const char *channel_name,
                                           daemon_l1_handler_fn handler, void *userdata)
{
    if (!channel_name || *channel_name == '\0' || !handler) {
        SVC_LOG_ERROR("daemon_l1_server_start: null/empty channel_name or handler");
        return NULL;
    }
    if (strlen(channel_name) >= DAEMON_L1_NAME_MAX) {
        SVC_LOG_ERROR("daemon_l1_server_start: channel name too long (%s)", channel_name);
        return NULL;
    }

    /* Idempotent (CAS swap inside corekern); daemons that already ran
     * airy_init() reach here initialized. */
    airy_err_t irc = airy_ipc_init();
    if (irc != AIRY_SUCCESS) {
        SVC_LOG_ERROR("daemon_l1_server_start: airy_ipc_init failed (%d)", irc);
        return NULL;
    }

    daemon_l1_server_t *svc = (daemon_l1_server_t *)AIRY_CALLOC(1, sizeof(*svc));
    if (!svc) {
        SVC_LOG_ERROR("daemon_l1_server_start: alloc failed");
        return NULL;
    }

    AIRY_STRNCPY_TERM(svc->name, channel_name, sizeof(svc->name));
    svc->handler = handler;
    svc->userdata = userdata;
    atomic_store_explicit(&svc->detached, false, memory_order_relaxed);
    atomic_store_explicit(&svc->inflight, 0, memory_order_relaxed);

    airy_err_t rc = airy_ipc_create_channel(svc->name, daemon_l1_bridge_cb, svc, &svc->channel);
    if (rc != AIRY_SUCCESS) {
        SVC_LOG_ERROR("daemon_l1_server_start: create_channel failed (%d) name=%s", rc,
                      svc->name);
        AIRY_FREE(svc);
        return NULL;
    }

    SVC_LOG_INFO("daemon_l1_server: mounted channel '%s' on corekern L1", svc->name);
    return svc;
}

void daemon_l1_server_stop(daemon_l1_server_t *svc)
{
    if (!svc) {
        return;
    }

    /* Detach first: new transactions fail fast inside the bridge. */
    atomic_store_explicit(&svc->detached, true, memory_order_release);

    if (svc->channel) {
        airy_ipc_close(svc->channel);
        svc->channel = NULL;
    }

    /* Drain: wait for transactions already inside the handler. The binder
     * invokes the handler without locks, so every in-flight call reaches
     * its counter decrement without further coordination. */
    while (atomic_load_explicit(&svc->inflight, memory_order_acquire) > 0) {
        airy_sleep_ms(1);
    }

    SVC_LOG_INFO("daemon_l1_server: unmounted channel '%s'", svc->name);
    AIRY_FREE(svc);
}

const char *daemon_l1_server_channel_name(const daemon_l1_server_t *svc)
{
    return svc ? svc->name : NULL;
}

bool daemon_l1_transport_enabled(const char *ns_upper)
{
    const char *value = NULL;
    char scoped[128];

    if (ns_upper && *ns_upper) {
        int n = snprintf(scoped, sizeof(scoped), "AIRY_%s_IPC_TRANSPORT", ns_upper);
        if (n > 0 && (size_t)n < sizeof(scoped)) {
            value = getenv(scoped);
            if (value && *value) {
                goto resolve;
            }
        }
    }
    value = getenv("AIRY_IPC_TRANSPORT");
    if (!value || *value == '\0') {
        return false; /* default: jsonrpc transport, L1 mount off */
    }

resolve:
    if (strcmp(value, "corekern") == 0) {
        return true;
    }
    if (strcmp(value, "jsonrpc") != 0) {
        SVC_LOG_WARN("daemon_l1_transport: unknown transport '%s' - fail-closed (off)", value);
    }
    return false;
}
