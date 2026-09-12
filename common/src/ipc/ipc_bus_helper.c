// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file ipc_bus_helper.c
 * @brief C-L09: IPC Bus -> daemon auto-registration convenience layer impl.
 *
 * Wraps the ipc_service_bus core API: one-call bus bring-up (create + start),
 * channel and handler registration, and request transport.
 *
 * 8.3.4 (0.1.15): the send/broadcast/notify/route/discover/backpressure
 * wrappers were removed together with the never-delivering bus family they
 * forwarded to; the helper is now registration + request only.
 *
 * @see ipc_bus_helper.h
 */

#include "ipc_bus_helper.h"

#include "airy_memory.h"
#include "safe_string_utils.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ipc_bus_helper_s {
    ipc_service_bus_t bus;
    ipc_bus_channel_t channel;
    char daemon_name[IPC_BUS_SERVICE_ID_LEN];
    bool channel_registered;
};

ipc_bus_helper_t *ipc_bus_helper_init(const char *daemon_name,
                                      const ipc_bus_channel_config_t *config)
{
    if (!daemon_name) {
        SVC_LOG_ERROR("ipc_bus_helper_init: daemon_name is NULL");
        return NULL;
    }

    ipc_bus_helper_t *ibh = (ipc_bus_helper_t *)AIRY_CALLOC(1, sizeof(ipc_bus_helper_t));
    if (!ibh) {
        SVC_LOG_ERROR("ipc_bus_helper_init: failed to allocate");
        return NULL;
    }

    safe_strcpy(ibh->daemon_name, daemon_name, sizeof(ibh->daemon_name));

    char bus_name[IPC_BUS_SERVICE_ID_LEN + 16];
    snprintf(bus_name, sizeof(bus_name), "bus-%s", daemon_name);

    ibh->bus = ipc_service_bus_create(bus_name, config);
    if (!ibh->bus) {
        SVC_LOG_ERROR("Failed to create IPC bus '%s'", bus_name);
        AIRY_FREE(ibh);
        return NULL;
    }

    airy_err_t err = ipc_service_bus_start(ibh->bus);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to start IPC bus '%s' (err=%d)", bus_name, err);
        ipc_service_bus_destroy(ibh->bus);
        AIRY_FREE(ibh);
        return NULL;
    }

    ibh->channel_registered = false;

    SVC_LOG_INFO("IPC bus helper initialized for daemon '%s'", daemon_name);
    return ibh;
}

void ipc_bus_helper_shutdown(ipc_bus_helper_t *ibh)
{
    if (!ibh)
        return;

    if (ibh->channel) {
        ipc_bus_channel_destroy(ibh->channel);
        ibh->channel = NULL;
    }

    if (ibh->bus) {
        ipc_service_bus_destroy(ibh->bus);
        ibh->bus = NULL;
    }

    SVC_LOG_INFO("IPC bus helper shutdown for daemon '%s'", ibh->daemon_name);
    AIRY_FREE(ibh);
}

int ipc_bus_helper_register_channel(ipc_bus_helper_t *ibh, const char *channel_name,
                                    ipc_bus_proto_t default_protocol)
{
    if (!ibh || !channel_name)
        return AIRY_ERR_INVALID_PARAM;

    if (ibh->channel_registered) {
        SVC_LOG_WARN("Channel already registered for daemon '%s'", ibh->daemon_name);
        return 0;
    }

    ipc_bus_channel_config_t ch_config;
    AIRY_MEMSET(&ch_config, 0, sizeof(ch_config));
    safe_strcpy(ch_config.name, channel_name, sizeof(ch_config.name));
    ch_config.default_protocol = default_protocol;
    ch_config.timeout_ms = IPC_BUS_DEFAULT_TIMEOUT_MS;
    ch_config.max_retries = IPC_BUS_MAX_RETRIES;
    ch_config.buffer_size = IPC_BUS_MAX_MESSAGE_SIZE;

    ibh->channel = ipc_bus_channel_create(ibh->bus, &ch_config);
    if (!ibh->channel) {
        SVC_LOG_ERROR("Failed to create channel '%s' for daemon '%s'", channel_name,
                      ibh->daemon_name);
        return AIRY_ERR_GENERIC_FAIL;
    }

    ibh->channel_registered = true;
    SVC_LOG_INFO("IPC channel '%s' registered for daemon '%s' (proto=%s)", channel_name,
                 ibh->daemon_name, ipc_bus_proto_to_string(default_protocol));
    return 0;
}

int ipc_bus_helper_register_handler(ipc_bus_helper_t *ibh, ipc_bus_message_handler_t handler,
                                    void *user_data)
{
    if (!ibh || !handler)
        return AIRY_ERR_INVALID_PARAM;

    airy_err_t err = ipc_service_bus_register_handler(ibh->bus, handler, user_data);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to register message handler for daemon '%s' (err=%d)",
                      ibh->daemon_name, err);
        return AIRY_ERR_GENERIC_FAIL;
    }

    SVC_LOG_INFO("Message handler registered for daemon '%s'", ibh->daemon_name);
    return 0;
}

int ipc_bus_helper_request(ipc_bus_helper_t *ibh, const char *target_service,
                           const ipc_bus_message_t *request, ipc_bus_message_t *response,
                           uint32_t timeout_ms)
{
    if (!ibh || !target_service || !request || !response)
        return AIRY_ERR_INVALID_PARAM;

    SVC_LOG_DEBUG("C-L09: REQUEST [%s] → [%s] timeout=%ums", ibh->daemon_name, target_service,
                  timeout_ms);

    airy_err_t err =
        ipc_service_bus_request(ibh->bus, target_service, request, response, timeout_ms);
    if (err == AIRY_SUCCESS) {
        SVC_LOG_DEBUG("C-L09: REQUEST OK [%s] → [%s]", ibh->daemon_name, target_service);
        return 0;
    } else {
        SVC_LOG_WARN("C-L09: REQUEST FAILED [%s] → [%s] err=%d", ibh->daemon_name, target_service,
                     (int)err);
        return AIRY_ERR_GENERIC_FAIL;
    }
}
