// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file ipc_service_bus.c
 * @brief IPC service-bus implementation - bus core domain.
 *
 * Implements an efficient communication-abstraction layer between daemons,
 * integrating the UnifiedProtocol stack, supporting multi-protocol message
 * passing, service discovery and load balancing.
 *
 * Phase 2.3a split: this file keeps the bus lifecycle (create/destroy/
 * start/stop), channel basics, send/request/broadcast/notify transport and
 * stats; the other domains were split out:
 * - handler/endpoint registry + discovery -> ipc_service_bus_endpoint.c
 * - message factory + header init         -> ipc_service_bus_message.c
 * Cross-file shared structs and init_message_header() are declared in
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
    bus->next_msg_id = 1;
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

    for (uint32_t i = 0; i < bus->pending_count; i++) {
        if (bus->pending[i].response) {
            ipc_bus_message_free(bus->pending[i].response);
        }
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

AIRY_API airy_err_t ipc_service_bus_send(ipc_service_bus_t bus_handle, const char *target_service,
                                         const ipc_bus_message_t *message)
{
    if (!bus_handle || !target_service || !message)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    if (!bus->running) {
        airy_mtx_unlock(&bus->mutex);
        return DAEMON_ESTATE;
    }

    bus->stats.messages_sent++;
    bus->stats.bytes_sent += message->payload_size;

    airy_mtx_unlock(&bus->mutex);

    AIRY_LOG_DEBUG("Bus '%s': sent message to '%s' (type=%d, proto=%d, size=%zu)", bus->name,
              target_service, message->header.msg_type, message->header.protocol,
              message->payload_size);
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

    if (bus->pending_count >= IPC_BUS_MAX_PENDING) {
        airy_mtx_unlock(&bus->mutex);
        return AIRY_EBUSY;
    }

    uint64_t start_time = airy_time_ms();

    pending_request_t *pending = &bus->pending[bus->pending_count];
    pending->msg_id = request->header.msg_id;
    pending->response = NULL;
    pending->completed = 0;
    bus->pending_count++;

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

    if (svc_err == AIRY_SUCCESS && resp_json) {
        size_t resp_len = strlen(resp_json) + 1;
        pending->response = (ipc_bus_message_t *)AIRY_CALLOC(1, sizeof(ipc_bus_message_t));
        if (pending->response) {
            pending->response->header.msg_type = IPC_BUS_MSG_RESPONSE;
            pending->response->header.protocol = request->header.protocol;
            snprintf(pending->response->header.target, sizeof(pending->response->header.target),
                     "%s", request->header.source);
            snprintf(pending->response->header.source, sizeof(pending->response->header.source),
                     "%s", target_service);
            pending->response->payload = resp_json;
            pending->response->payload_size = resp_len;
            pending->completed = 1;
        } else {
            AIRY_FREE(resp_json);
            resp_json = NULL;
            pending->completed = 0;
        }
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

    uint64_t elapsed = airy_time_ms() - start_time;
    if (elapsed >= (uint64_t)timeout_ms && !pending->completed) {
        bus->stats.timeouts++;
        bus->pending_count--;
        if (resp_json)
            AIRY_FREE(resp_json);
        airy_mtx_unlock(&bus->mutex);
        return AIRY_ETIMEDOUT;
    }

    if (pending->completed && pending->response) {
        /* Transfer the response message (including payload ownership) to the
         * caller. Note: only the pending->response struct itself is freed
         * here; payload ownership goes to the caller, who must release it via
         * AIRY_FREE(response->payload) after use. ipc_bus_message_free() must
         * NOT be called, since it also frees the payload and would leave the
         * caller's response->payload as a dangling pointer (use-after-free).
         * All callers (orchestrator.c, daemon_task_dispatcher.c,
         * ipc_bus_helper.c) already follow the "caller frees response.payload"
         * contract. */
        __builtin_memcpy(response, pending->response, sizeof(ipc_bus_message_t));
        AIRY_FREE(pending->response);
        pending->response = NULL;
    }

    bus->pending_count--;
    bus->stats.messages_received++;
    uint64_t latency = airy_time_ms() - start_time;
    bus->stats.avg_latency_us = bus->stats.avg_latency_us == 0 ?
                                    latency * 1000 :
                                    (bus->stats.avg_latency_us + latency * 1000) / 2;
    if (latency * 1000 > bus->stats.max_latency_us)
        bus->stats.max_latency_us = latency * 1000;

    airy_mtx_unlock(&bus->mutex);

    AIRY_LOG_DEBUG("Bus '%s': request to '%s' completed in %llums (completed=%d)", bus->name,
              target_service, (unsigned long long)latency, pending->completed);
    /* RPC success returns SUCCESS; RPC failure returns svc_err (AIRY_EIO
     * etc.), following the API contract "0=success, non-zero=failure". */
    return svc_err;
}

AIRY_API airy_err_t ipc_service_bus_broadcast(ipc_service_bus_t bus_handle,
                                              const ipc_bus_message_t *message)
{
    if (!bus_handle || !message)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    if (!bus->running) {
        airy_mtx_unlock(&bus->mutex);
        return DAEMON_ESTATE;
    }

    uint32_t target_count = 0;
    for (uint32_t i = 0; i < bus->endpoint_count; i++) {
        if (bus->endpoints[i].healthy)
            target_count++;
    }

    bus->stats.messages_sent += target_count;
    bus->stats.bytes_sent += message->payload_size * target_count;

    airy_mtx_unlock(&bus->mutex);

    airy_err_t first_error = AIRY_SUCCESS;
    uint32_t sent_count = 0;
    for (uint32_t i = 0; i < bus->endpoint_count; i++) {
        if (bus->endpoints[i].healthy) {
            airy_err_t err =
                ipc_service_bus_send(bus_handle, bus->endpoints[i].service_name, message);
            if (err == AIRY_SUCCESS) {
                sent_count++;
            } else if (first_error == AIRY_SUCCESS) {
                first_error = err;
            }
        }
    }

    AIRY_LOG_DEBUG("Bus '%s': broadcast to %u/%u endpoints succeeded", bus->name, sent_count,
              target_count);
    return (sent_count > 0) ? AIRY_SUCCESS : first_error;
}

AIRY_API airy_err_t ipc_service_bus_notify(ipc_service_bus_t bus_handle, const char *target_service,
                                           const void *payload, size_t payload_size,
                                           ipc_bus_proto_t protocol)
{
    if (!bus_handle || !target_service || !payload)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    ipc_bus_message_t *msg =
        ipc_bus_message_create(IPC_BUS_MSG_NOTIFICATION, protocol, payload, payload_size);
    if (!msg)
        return AIRY_ENOMEM;

    init_message_header(&msg->header, IPC_BUS_MSG_NOTIFICATION, protocol, bus->name,
                        target_service);
    msg->header.aipc.payload_len = (uint32_t)payload_size;

    airy_err_t err = ipc_service_bus_send(bus_handle, target_service, msg);
    ipc_bus_message_free(msg);

    return err;
}

AIRY_API airy_err_t ipc_service_bus_get_stats(ipc_service_bus_t bus_handle, ipc_bus_stats_t *stats)
{
    if (!bus_handle || !stats)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);
    __builtin_memcpy(stats, &bus->stats, sizeof(ipc_bus_stats_t));
    stats->active_channels = bus->channel_count;
    stats->active_endpoints = bus->endpoint_count;
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
