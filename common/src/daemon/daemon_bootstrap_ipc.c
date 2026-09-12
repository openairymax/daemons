// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_bootstrap_ipc.c
 * @brief P1.8 C-L09: daemon IPC Bus one-call bootstrap implementation.
 *
 * @see daemon_bootstrap_ipc.h
 * @see P1.8 C-L09 wiring
 */

#include "daemon_bootstrap_ipc.h"

#include "airy_memory.h"
#include "svc_logger.h"

#include <stdio.h>
#include <string.h>

struct daemon_bootstrap_ipc_s {
    ipc_bus_helper_t *ibh;
    bool running;
    char daemon_name[64];
    char channel_name[64];
    ipc_bus_proto_t protocol;
};

daemon_bootstrap_ipc_t *daemon_bootstrap_ipc_start(const char *daemon_name,
                                                   const char *channel_name, const char *host,
                                                   uint16_t port, ipc_bus_proto_t protocol)
{
    /* host/port stay in the signature for compatibility with the atoms ops
     * table; the bus has no endpoint registry since 8.3.4 (0.1.15). */
    (void)host;
    (void)port;

    if (!daemon_name || !channel_name) {
        SVC_LOG_ERROR("daemon_bootstrap_ipc_start: invalid parameters");
        return NULL;
    }

    daemon_bootstrap_ipc_t *bipc =
        (daemon_bootstrap_ipc_t *)AIRY_CALLOC(1, sizeof(daemon_bootstrap_ipc_t));
    if (!bipc)
        return NULL;

    bipc->ibh = ipc_bus_helper_init(daemon_name, NULL);
    if (!bipc->ibh) {
        SVC_LOG_ERROR("daemon_bootstrap_ipc_start: init failed for '%s'", daemon_name);
        AIRY_FREE(bipc);
        return NULL;
    }

    if (ipc_bus_helper_register_channel(bipc->ibh, channel_name, protocol) != 0) {
        SVC_LOG_ERROR("daemon_bootstrap_ipc_start: channel register failed for '%s'", daemon_name);
        ipc_bus_helper_shutdown(bipc->ibh);
        AIRY_FREE(bipc);
        return NULL;
    }

    AIRY_STRNCPY_TERM(bipc->daemon_name, daemon_name, sizeof(bipc->daemon_name) - 1);
    AIRY_STRNCPY_TERM(bipc->channel_name, channel_name, sizeof(bipc->channel_name) - 1);
    bipc->protocol = protocol;
    bipc->running = true;

    SVC_LOG_INFO("C-L09: IPC Bus bootstrapped for '%s' (channel=%s, proto=%s)", daemon_name,
                 channel_name, ipc_bus_proto_to_string(protocol));
    return bipc;
}

daemon_bootstrap_ipc_t *daemon_bootstrap_ipc_start_unix(const char *daemon_name,
                                                        const char *channel_name,
                                                        const char *socket_path,
                                                        ipc_bus_proto_t protocol)
{
    if (!daemon_name || !channel_name || !socket_path) {
        SVC_LOG_ERROR("daemon_bootstrap_ipc_start_unix: invalid parameters");
        return NULL;
    }

    daemon_bootstrap_ipc_t *bipc =
        (daemon_bootstrap_ipc_t *)AIRY_CALLOC(1, sizeof(daemon_bootstrap_ipc_t));
    if (!bipc)
        return NULL;

    bipc->ibh = ipc_bus_helper_init(daemon_name, NULL);
    if (!bipc->ibh) {
        SVC_LOG_ERROR("daemon_bootstrap_ipc_start_unix: init failed for '%s'", daemon_name);
        AIRY_FREE(bipc);
        return NULL;
    }

    if (ipc_bus_helper_register_channel(bipc->ibh, channel_name, protocol) != 0) {
        ipc_bus_helper_shutdown(bipc->ibh);
        AIRY_FREE(bipc);
        return NULL;
    }

    AIRY_STRNCPY_TERM(bipc->daemon_name, daemon_name, sizeof(bipc->daemon_name) - 1);
    AIRY_STRNCPY_TERM(bipc->channel_name, channel_name, sizeof(bipc->channel_name) - 1);
    bipc->protocol = protocol;
    bipc->running = true;

    SVC_LOG_INFO("C-L09: IPC Bus bootstrapped for '%s' (unix:%s, proto=%s)", daemon_name,
                 socket_path, ipc_bus_proto_to_string(protocol));
    return bipc;
}

void daemon_bootstrap_ipc_stop(daemon_bootstrap_ipc_t *bipc)
{
    if (!bipc)
        return;

    SVC_LOG_INFO("C-L09: IPC Bus shutting down for '%s'", bipc->daemon_name);

    if (bipc->ibh) {
        ipc_bus_helper_shutdown(bipc->ibh);
        bipc->ibh = NULL;
    }

    bipc->running = false;
    AIRY_FREE(bipc);
}

int daemon_bootstrap_ipc_register_handler(daemon_bootstrap_ipc_t *bipc,
                                          ipc_bus_message_handler_t handler, void *user_data)
{
    if (!bipc || !handler)
        return AIRY_ERR_INVALID_PARAM;
    return ipc_bus_helper_register_handler(bipc->ibh, handler, user_data);
}

ipc_bus_helper_t *daemon_bootstrap_ipc_get_helper(daemon_bootstrap_ipc_t *bipc)
{
    return bipc ? bipc->ibh : NULL;
}

bool daemon_bootstrap_ipc_is_running(daemon_bootstrap_ipc_t *bipc)
{
    return bipc ? bipc->running : false;
}