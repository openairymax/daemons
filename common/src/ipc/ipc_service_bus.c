// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file ipc_service_bus.c
 * @brief IPC service-bus implementation - bus core domain.
 *
 * Implements the daemon request transport on top of svc_rpc_call (the L2
 * JSON-RPC wire), plus bus lifecycle, channel basics, the message handler
 * registry and stats.
 *
 * 8.3.4 (0.1.15): the never-delivering send/broadcast/notify family and the
 * endpoint/event registry ceremonies were removed; the only real delivery
 * path is ipc_service_bus_request() -> svc_rpc_call(). Handler registration
 * moved here when ipc_service_bus_endpoint.c was retired. The message
 * factory stays in ipc_service_bus_message.c; shared structs live in
 * ipc_service_bus_internal.h (internal to this static lib, not public API).
 *
 * @see ipc_service_bus.h
 * @see agentrt/daemons/common/src/ipc_service_bus_internal.h
 */

#include "ipc_service_bus.h"
#include "ipc_service_bus_internal.h"

#include "ipc_client.h"
#include "airy_memory.h"
#include "safe_string_utils.h"
#include "svc_common.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "error.h"

#include "daemon_errors.h"

static uint64_t g_bus_instance_count = 0;

static ipc_bus_channel_internal_t *find_channel(ipc_service_bus_internal_t *bus, const char *name)
{
    ipc_bus_channel_internal_t *ch = bus->channels;
    while (ch) {
        if (strcmp(ch->name, name) == 0)
            return ch;
        ch = ch->next;
    }
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

AIRY_API ipc_service_bus_t ipc_service_bus_create(const char *bus_name,
                                                  const ipc_bus_channel_config_t *config)
{
    if (!bus_name) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    ipc_service_bus_internal_t *bus =
        (ipc_service_bus_internal_t *)AIRY_CALLOC(1, sizeof(ipc_service_bus_internal_t));
    if (!bus) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    if (safe_strcpy(bus->name, bus_name, IPC_BUS_SERVICE_ID_LEN) != 0) {
        AIRY_FREE(bus);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    if (config) {
        __builtin_memcpy(&bus->default_config, config, sizeof(ipc_bus_channel_config_t));
    } else {
        AIRY_MEMSET(&bus->default_config, 0, sizeof(ipc_bus_channel_config_t));
        safe_strcpy(bus->default_config.name, "default", IPC_BUS_CHANNEL_NAME_LEN);
        bus->default_config.default_protocol = IPC_BUS_PROTO_JSON_RPC;
        bus->default_config.timeout_ms = IPC_BUS_DEFAULT_TIMEOUT_MS;
        bus->default_config.max_retries = IPC_BUS_MAX_RETRIES;
        bus->default_config.buffer_size = IPC_BUS_MAX_MESSAGE_SIZE;
    }

    airy_err_t err = airy_mtx_init(&bus->mutex);
    if (err != AIRY_SUCCESS) {
        AIRY_FREE(bus);
        AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
    }

    bus->running = false;
    g_bus_instance_count++;

    AIRY_LOG_INFO("IPC service bus '%s' created", bus_name);
    return (ipc_service_bus_t)bus;
}

AIRY_API void ipc_service_bus_destroy(ipc_service_bus_t bus_handle)
{
    if (!bus_handle)
        return;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    if (bus->running) {
        ipc_service_bus_stop(bus_handle);
    }

    ipc_bus_channel_internal_t *ch = bus->channels;
    while (ch) {
        ipc_bus_channel_internal_t *next = ch->next;
        AIRY_FREE(ch);
        ch = next;
    }

    airy_mtx_destroy(&bus->mutex);
    AIRY_FREE(bus);

    AIRY_LOG_INFO("IPC service bus destroyed");
}

AIRY_API airy_err_t ipc_service_bus_start(ipc_service_bus_t bus_handle)
{
    if (!bus_handle)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);
    if (bus->running) {
        airy_mtx_unlock(&bus->mutex);
        return AIRY_SUCCESS;
    }

    bus->running = true;
    airy_mtx_unlock(&bus->mutex);

    AIRY_LOG_INFO("IPC service bus '%s' started", bus->name);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t ipc_service_bus_stop(ipc_service_bus_t bus_handle)
{
    if (!bus_handle)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);
    bus->running = false;
    airy_mtx_unlock(&bus->mutex);

    AIRY_LOG_INFO("IPC service bus '%s' stopped", bus->name);
    return AIRY_SUCCESS;
}

AIRY_API ipc_bus_channel_t ipc_bus_channel_create(ipc_service_bus_t bus_handle,
                                                  const ipc_bus_channel_config_t *config)
{
    if (!bus_handle || !config) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    if (bus->channel_count >= IPC_BUS_MAX_CHANNELS) {
        airy_mtx_unlock(&bus->mutex);
        AIRY_LOG_ERROR("Cannot create channel: max channels reached");
        AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
    }

    if (find_channel(bus, config->name)) {
        airy_mtx_unlock(&bus->mutex);
        AIRY_LOG_ERROR("Channel '%s' already exists", config->name);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
    }

    ipc_bus_channel_internal_t *ch =
        (ipc_bus_channel_internal_t *)AIRY_CALLOC(1, sizeof(ipc_bus_channel_internal_t));
    if (!ch) {
        airy_mtx_unlock(&bus->mutex);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    __builtin_memcpy(&ch->config, config, sizeof(ipc_bus_channel_config_t));
    safe_strcpy(ch->name, config->name, IPC_BUS_CHANNEL_NAME_LEN);
    ch->active = true;
    ch->next = bus->channels;
    bus->channels = ch;
    bus->channel_count++;

    airy_mtx_unlock(&bus->mutex);

    AIRY_LOG_INFO("Channel '%s' created on bus '%s'", config->name, bus->name);
    return (ipc_bus_channel_t)ch;
}

AIRY_API void ipc_bus_channel_destroy(ipc_bus_channel_t channel)
{
    if (!channel)
        return;

    ipc_bus_channel_internal_t *ch = (ipc_bus_channel_internal_t *)channel;
    ch->active = false;

    AIRY_LOG_INFO("Channel '%s' destroyed", ch->name);
}

AIRY_API const char *ipc_bus_channel_get_name(ipc_bus_channel_t channel)
{
    if (!channel) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    ipc_bus_channel_internal_t *ch = (ipc_bus_channel_internal_t *)channel;
    return ch->name;
}

AIRY_API airy_err_t ipc_service_bus_register_handler(ipc_service_bus_t bus_handle,
                                                     ipc_bus_message_handler_t handler,
                                                     void *user_data)
{
    if (!bus_handle || !handler)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    if (bus->channel_count == 0) {
        ipc_bus_channel_config_t config;
        __builtin_memcpy(&config, &bus->default_config, sizeof(ipc_bus_channel_config_t));
        safe_strcpy(config.name, "default", IPC_BUS_CHANNEL_NAME_LEN);
        airy_mtx_unlock(&bus->mutex);

        ipc_bus_channel_t ch = ipc_bus_channel_create(bus_handle, &config);
        if (!ch)
            return AIRY_ENOMEM;

        airy_mtx_lock(&bus->mutex);
    }

    ipc_bus_channel_internal_t *ch = bus->channels;
    if (!ch || ch->handler_count >= IPC_BUS_MAX_HANDLERS) {
        airy_mtx_unlock(&bus->mutex);
        return AIRY_ENOMEM;
    }

    ch->handlers[ch->handler_count].handler = handler;
    ch->handlers[ch->handler_count].user_data = user_data;
    ch->handler_count++;

    airy_mtx_unlock(&bus->mutex);

    AIRY_LOG_INFO("Message handler registered on bus '%s'", bus->name);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t ipc_service_bus_unregister_handler(ipc_service_bus_t bus_handle,
                                                       ipc_bus_message_handler_t handler)
{
    if (!bus_handle || !handler)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    ipc_bus_channel_internal_t *ch = bus->channels;
    while (ch) {
        for (uint32_t i = 0; i < ch->handler_count; i++) {
            if (ch->handlers[i].handler == handler) {
                if (i < ch->handler_count - 1) {
                    ch->handlers[i] = ch->handlers[ch->handler_count - 1];
                }
                ch->handler_count--;
                break;
            }
        }
        ch = ch->next;
    }

    airy_mtx_unlock(&bus->mutex);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t ipc_service_bus_request(ipc_service_bus_t bus_handle,
                                            const char *target_service,
                                            const ipc_bus_message_t *request,
                                            ipc_bus_message_t *response, uint32_t timeout_ms)
{
    if (!bus_handle || !target_service || !request || !response)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    if (!bus->running) {
        airy_mtx_unlock(&bus->mutex);
        return DAEMON_ESTATE;
    }

    if (bus->inflight_requests >= IPC_BUS_MAX_INFLIGHT) {
        airy_mtx_unlock(&bus->mutex);
        return AIRY_EBUSY;
    }
    bus->inflight_requests++;

    uint64_t start_time = airy_time_ms();

    bus->stats.messages_sent++;
    bus->stats.bytes_sent += request->payload_size;

    airy_mtx_unlock(&bus->mutex);

    if (timeout_ms == 0)
        timeout_ms = bus->default_config.timeout_ms;

    const char *req_payload = (const char *)request->payload;
    char *resp_json = NULL;
    airy_err_t svc_err = AIRY_SUCCESS;

    char rpc_method[256];
    snprintf(rpc_method, sizeof(rpc_method), "%s.handle", target_service);

    int rpc_err =
        svc_rpc_call(rpc_method, req_payload ? req_payload : "{}", &resp_json, timeout_ms);
    if (rpc_err != 0) {
        svc_err = AIRY_EIO;
    }

    airy_mtx_lock(&bus->mutex);
    bus->inflight_requests--;

    if (svc_err == AIRY_SUCCESS && resp_json) {
        /* Build the response message in the caller's storage (zeroed first,
         * mirroring the factory guarantee). Payload ownership moves into it:
         * the caller must release it via AIRY_FREE(response->payload) after
         * use. ipc_bus_message_free() must NOT be called on the caller's
         * stack copy, since it would free a stack address. All callers
         * (orchestrator.c, daemon_task_dispatcher.c, ipc_bus_helper.c)
         * already follow the "caller frees response.payload" contract. */
        AIRY_MEMSET(response, 0, sizeof(*response));
        response->header.msg_type = IPC_BUS_MSG_RESPONSE;
        response->header.protocol = request->header.protocol;
        snprintf(response->header.target, sizeof(response->header.target), "%s",
                 request->header.source);
        snprintf(response->header.source, sizeof(response->header.source), "%s", target_service);
        response->payload = resp_json;
        response->payload_size = strlen(resp_json) + 1;
    } else {
        /* RPC call failed: do not create an error response message; the
         * function returns svc_err to indicate transport failure. The caller
         * distinguishes "transport failure" (err != SUCCESS, response not
         * filled) from "business error" (err == SUCCESS, response.payload
         * carries the business-layer error info). Previously an
         * {"error":{"code":...}} message was created here and SUCCESS
         * returned, conflating transport errors with business errors and
         * violating the API contract "0=success, non-zero=failure". */
        if (resp_json) {
            AIRY_FREE(resp_json);
            resp_json = NULL;
        }
    }

    bus->stats.messages_received++;
    uint64_t latency = airy_time_ms() - start_time;
    bus->stats.avg_latency_us = bus->stats.avg_latency_us == 0 ?
                                    latency * 1000 :
                                    (bus->stats.avg_latency_us + latency * 1000) / 2;
    if (latency * 1000 > bus->stats.max_latency_us)
        bus->stats.max_latency_us = latency * 1000;

    airy_mtx_unlock(&bus->mutex);

    AIRY_LOG_DEBUG("Bus '%s': request to '%s' completed in %llums", bus->name, target_service,
              (unsigned long long)latency);
    /* RPC success returns SUCCESS; RPC failure returns svc_err (AIRY_EIO
     * etc.), following the API contract "0=success, non-zero=failure". */
    return svc_err;
}

AIRY_API airy_err_t ipc_service_bus_get_stats(ipc_service_bus_t bus_handle, ipc_bus_stats_t *stats)
{
    if (!bus_handle || !stats)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);
    __builtin_memcpy(stats, &bus->stats, sizeof(ipc_bus_stats_t));
    stats->active_channels = bus->channel_count;
    airy_mtx_unlock(&bus->mutex);

    return AIRY_SUCCESS;
}

AIRY_API airy_err_t ipc_service_bus_reset_stats(ipc_service_bus_t bus_handle)
{
    if (!bus_handle)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);
    AIRY_MEMSET(&bus->stats, 0, sizeof(ipc_bus_stats_t));
    airy_mtx_unlock(&bus->mutex);

    return AIRY_SUCCESS;
}

AIRY_API const char *ipc_service_bus_get_name(ipc_service_bus_t bus_handle)
{
    if (!bus_handle) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;
    return bus->name;
}

AIRY_API bool ipc_service_bus_is_running(ipc_service_bus_t bus_handle)
{
    if (!bus_handle)
        return false;
    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;
    return bus->running;
}
