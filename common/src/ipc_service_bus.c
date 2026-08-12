// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file ipc_service_bus.c
 * @brief IPC service-bus implementation - unified inter-daemon comm framework.
 *
 * Implements an efficient communication-abstraction layer between daemons,
 * integrating the UnifiedProtocol stack, supporting multi-protocol message
 * passing, service discovery and load balancing.
 *
 * @see ipc_service_bus.h
 */

#include "ipc_service_bus.h"

#include "atomic_compat.h"
#include "ipc_client.h"
#include "airy_memory.h"
#include "platform.h"
#include "safe_string_utils.h"
#include "svc_common.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "error.h"

#include "daemon_errors.h"

#define IPC_BUS_MAX_HANDLERS 16
#define IPC_BUS_MAX_EVENTS 32
#define IPC_BUS_MAX_PENDING 256
#define IPC_BUS_HASH_SEED 0x9e3779b9

typedef struct {
    ipc_bus_message_handler_t handler;
    void *user_data;
} message_handler_entry_t;

typedef struct {
    char event_name[64];
    ipc_bus_event_handler_t handler;
    void *user_data;
} event_handler_entry_t;

typedef struct {
    uint64_t msg_id;
    ipc_bus_message_t *response;
    atomic_int completed;
    airy_mtx_t mutex;
    airy_cond_t cond;
} pending_request_t;

typedef struct ipc_bus_channel_s {
    char name[IPC_BUS_CHANNEL_NAME_LEN];
    ipc_bus_channel_config_t config;
    message_handler_entry_t handlers[IPC_BUS_MAX_HANDLERS];
    uint32_t handler_count;
    bool active;
    struct ipc_bus_channel_s *next;
} ipc_bus_channel_internal_t;

typedef struct ipc_service_bus_s {
    char name[IPC_BUS_SERVICE_ID_LEN];
    ipc_bus_channel_config_t default_config;
    ipc_bus_endpoint_t endpoints[IPC_BUS_MAX_SERVICES];
    uint32_t endpoint_count;
    ipc_bus_channel_internal_t *channels;
    uint32_t channel_count;
    event_handler_entry_t event_handlers[IPC_BUS_MAX_EVENTS];
    uint32_t event_handler_count;
    pending_request_t pending[IPC_BUS_MAX_PENDING];
    uint32_t pending_count;
    ipc_bus_stats_t stats;
    bool running;
    airy_mtx_t mutex;
    uint64_t next_msg_id;
} ipc_service_bus_internal_t;

static uint64_t g_bus_instance_count = 0;

static uint64_t __attribute__((unused)) generate_msg_id(ipc_service_bus_internal_t *bus)
{
    uint64_t id = bus->next_msg_id++;
    if (bus->next_msg_id == 0)
        bus->next_msg_id = 1;
    return id;
}

static uint32_t compute_checksum(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

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

static int32_t find_endpoint_index(ipc_service_bus_internal_t *bus, const char *service_name)
{
    for (uint32_t i = 0; i < bus->endpoint_count; i++) {
        if (strcmp(bus->endpoints[i].service_name, service_name) == 0)
            return (int32_t)i;
    }
    return AIRY_ERR_NOT_FOUND;
}

static void init_message_header(ipc_bus_message_header_t *header, ipc_bus_msg_type_t msg_type,
                                ipc_bus_proto_t protocol, const char *source, const char *target)
{
    __builtin_memset(header, 0, sizeof(ipc_bus_message_header_t));
    header->magic = IPC_BUS_MESSAGE_MAGIC;
    header->version = IPC_BUS_MESSAGE_VERSION;
    header->msg_type = msg_type;
    header->protocol = protocol;
    header->timestamp = airy_time_ms();
    if (source)
        safe_strcpy(header->source, source, IPC_BUS_SERVICE_ID_LEN);
    if (target)
        safe_strcpy(header->target, target, IPC_BUS_SERVICE_ID_LEN);
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
        __builtin_memset(&bus->default_config, 0, sizeof(ipc_bus_channel_config_t));
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

    LOG_INFO("IPC service bus '%s' created", bus_name);
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

    LOG_INFO("IPC service bus destroyed");
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

    LOG_INFO("IPC service bus '%s' started", bus->name);
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

    LOG_INFO("IPC service bus '%s' stopped", bus->name);
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
        LOG_ERROR("Cannot create channel: max channels reached");
        AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
    }

    if (find_channel(bus, config->name)) {
        airy_mtx_unlock(&bus->mutex);
        LOG_ERROR("Channel '%s' already exists", config->name);
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

    LOG_INFO("Channel '%s' created on bus '%s'", config->name, bus->name);
    return (ipc_bus_channel_t)ch;
}

AIRY_API void ipc_bus_channel_destroy(ipc_bus_channel_t channel)
{
    if (!channel)
        return;

    ipc_bus_channel_internal_t *ch = (ipc_bus_channel_internal_t *)channel;
    ch->active = false;

    LOG_INFO("Channel '%s' destroyed", ch->name);
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

    LOG_DEBUG("Bus '%s': sent message to '%s' (type=%d, proto=%d, size=%zu)", bus->name,
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

    LOG_DEBUG("Bus '%s': request to '%s' completed in %llums (completed=%d)", bus->name,
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

    LOG_DEBUG("Bus '%s': broadcast to %u/%u endpoints succeeded", bus->name, sent_count,
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
    msg->header.payload_len = (uint32_t)payload_size;

    airy_err_t err = ipc_service_bus_send(bus_handle, target_service, msg);
    ipc_bus_message_free(msg);

    return err;
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

    LOG_INFO("Message handler registered on bus '%s'", bus->name);
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

AIRY_API airy_err_t ipc_service_bus_register_event_handler(ipc_service_bus_t bus_handle,
                                                           const char *event_name,
                                                           ipc_bus_event_handler_t handler,
                                                           void *user_data)
{
    if (!bus_handle || !event_name || !handler)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    if (bus->event_handler_count >= IPC_BUS_MAX_EVENTS) {
        airy_mtx_unlock(&bus->mutex);
        return AIRY_ENOMEM;
    }

    event_handler_entry_t *entry = &bus->event_handlers[bus->event_handler_count];
    safe_strcpy(entry->event_name, event_name, sizeof(entry->event_name));
    entry->handler = handler;
    entry->user_data = user_data;
    bus->event_handler_count++;

    airy_mtx_unlock(&bus->mutex);

    LOG_INFO("Event handler registered for '%s' on bus '%s'", event_name, bus->name);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t ipc_service_bus_register_endpoint(ipc_service_bus_t bus_handle,
                                                      const ipc_bus_endpoint_t *endpoint)
{
    if (!bus_handle || !endpoint)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    int32_t idx = find_endpoint_index(bus, endpoint->service_name);
    if (idx >= 0) {
        __builtin_memcpy(&bus->endpoints[idx], endpoint, sizeof(ipc_bus_endpoint_t));
        bus->endpoints[idx].last_heartbeat = airy_time_ms();
        airy_mtx_unlock(&bus->mutex);
        LOG_INFO("Endpoint '%s' updated on bus '%s'", endpoint->service_name, bus->name);
        return AIRY_SUCCESS;
    }

    if (bus->endpoint_count >= IPC_BUS_MAX_SERVICES) {
        airy_mtx_unlock(&bus->mutex);
        return AIRY_ENOMEM;
    }

    __builtin_memcpy(&bus->endpoints[bus->endpoint_count], endpoint, sizeof(ipc_bus_endpoint_t));
    bus->endpoints[bus->endpoint_count].last_heartbeat = airy_time_ms();
    bus->endpoint_count++;
    bus->stats.active_endpoints = bus->endpoint_count;

    airy_mtx_unlock(&bus->mutex);

    LOG_INFO("Endpoint '%s' registered on bus '%s' (endpoint=%s)", endpoint->service_name,
             bus->name, endpoint->endpoint);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t ipc_service_bus_unregister_endpoint(ipc_service_bus_t bus_handle,
                                                        const char *service_name)
{
    if (!bus_handle || !service_name)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    int32_t idx = find_endpoint_index(bus, service_name);
    if (idx < 0) {
        airy_mtx_unlock(&bus->mutex);
        return AIRY_ENOENT;
    }

    if ((uint32_t)idx < bus->endpoint_count - 1) {
        bus->endpoints[idx] = bus->endpoints[bus->endpoint_count - 1];
    }
    __builtin_memset(&bus->endpoints[bus->endpoint_count - 1], 0, sizeof(ipc_bus_endpoint_t));
    bus->endpoint_count--;
    bus->stats.active_endpoints = bus->endpoint_count;

    airy_mtx_unlock(&bus->mutex);

    LOG_INFO("Endpoint '%s' unregistered from bus '%s'", service_name, bus->name);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t ipc_service_bus_discover(ipc_service_bus_t bus_handle, const char *service_name,
                                             ipc_bus_proto_t protocol,
                                             ipc_bus_endpoint_t *endpoints, uint32_t max_count,
                                             uint32_t *found_count)
{
    if (!bus_handle || !endpoints || !found_count)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    uint32_t count = 0;
    for (uint32_t i = 0; i < bus->endpoint_count && count < max_count; i++) {
        ipc_bus_endpoint_t *ep = &bus->endpoints[i];

        if (service_name && service_name[0] && strcmp(ep->service_name, service_name) != 0)
            continue;

        if (protocol != IPC_BUS_PROTO_AUTO) {
            bool proto_match = false;
            for (uint32_t p = 0; p < ep->protocol_count; p++) {
                if (ep->supported_protocols[p] == protocol) {
                    proto_match = true;
                    break;
                }
            }
            if (!proto_match)
                continue;
        }

        __builtin_memcpy(&endpoints[count], ep, sizeof(ipc_bus_endpoint_t));
        count++;
    }

    *found_count = count;
    airy_mtx_unlock(&bus->mutex);

    LOG_DEBUG("Service discovery: found %u endpoints (name=%s, proto=%d)", count,
              service_name ? service_name : "*", protocol);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t ipc_service_bus_select_endpoint(ipc_service_bus_t bus_handle,
                                                    const char *service_name,
                                                    ipc_bus_proto_t protocol,
                                                    ipc_bus_endpoint_t *endpoint)
{
    if (!bus_handle || !service_name || !endpoint)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    ipc_bus_endpoint_t *best = NULL;
    uint32_t best_load = UINT32_MAX;

    for (uint32_t i = 0; i < bus->endpoint_count; i++) {
        ipc_bus_endpoint_t *ep = &bus->endpoints[i];

        if (strcmp(ep->service_name, service_name) != 0)
            continue;
        if (!ep->healthy)
            continue;

        if (protocol != IPC_BUS_PROTO_AUTO) {
            bool proto_match = false;
            for (uint32_t p = 0; p < ep->protocol_count; p++) {
                if (ep->supported_protocols[p] == protocol) {
                    proto_match = true;
                    break;
                }
            }
            if (!proto_match)
                continue;
        }

        uint32_t load =
            ep->max_connections > 0 ? ep->active_connections * 100 / ep->max_connections : 0;
        if (ep->weight > 0)
            load = load / ep->weight;

        if (load < best_load) {
            best_load = load;
            best = ep;
        }
    }

    if (!best) {
        airy_mtx_unlock(&bus->mutex);
        return AIRY_ENOENT;
    }

    __builtin_memcpy(endpoint, best, sizeof(ipc_bus_endpoint_t));
    airy_mtx_unlock(&bus->mutex);

    return AIRY_SUCCESS;
}

AIRY_API airy_err_t ipc_service_bus_update_endpoint_health(ipc_service_bus_t bus_handle,
                                                           const char *service_name, bool healthy)
{
    if (!bus_handle || !service_name)
        return AIRY_EINVAL;

    ipc_service_bus_internal_t *bus = (ipc_service_bus_internal_t *)bus_handle;

    airy_mtx_lock(&bus->mutex);

    int32_t idx = find_endpoint_index(bus, service_name);
    if (idx < 0) {
        airy_mtx_unlock(&bus->mutex);
        return AIRY_ENOENT;
    }

    bool was_healthy = bus->endpoints[idx].healthy;
    bus->endpoints[idx].healthy = healthy;
    bus->endpoints[idx].last_heartbeat = airy_time_ms();

    airy_mtx_unlock(&bus->mutex);

    if (was_healthy && !healthy) {
        LOG_WARN("Endpoint '%s' became unhealthy on bus '%s'", service_name, bus->name);
    } else if (!was_healthy && healthy) {
        LOG_INFO("Endpoint '%s' recovered on bus '%s'", service_name, bus->name);
    }

    return AIRY_SUCCESS;
}

AIRY_API ipc_bus_message_t *ipc_bus_message_create(ipc_bus_msg_type_t msg_type,
                                                   ipc_bus_proto_t protocol, const void *payload,
                                                   size_t payload_size)
{
    ipc_bus_message_t *msg = (ipc_bus_message_t *)AIRY_CALLOC(1, sizeof(ipc_bus_message_t));
    if (!msg) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    init_message_header(&msg->header, msg_type, protocol, NULL, NULL);
    msg->header.msg_id = (uint64_t)airy_time_ms();
    msg->header.payload_len = (uint32_t)payload_size;

    if (payload && payload_size > 0) {
        msg->payload = AIRY_CALLOC(1, payload_size);
        if (!msg->payload) {
            AIRY_FREE(msg);
            AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
        }

        __builtin_memcpy(msg->payload, payload, payload_size);
        msg->payload_size = payload_size;
        msg->header.checksum = compute_checksum(payload, payload_size);
    }

    return msg;
}

AIRY_API void ipc_bus_message_free(ipc_bus_message_t *message)
{
    if (!message)
        return;
    if (message->payload) {
        AIRY_FREE(message->payload);
        message->payload = NULL;
    }
    AIRY_FREE(message);
}

AIRY_API ipc_bus_message_t *ipc_bus_message_clone(const ipc_bus_message_t *message)
{
    if (!message) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    ipc_bus_message_t *clone =
        ipc_bus_message_create(message->header.msg_type, message->header.protocol, message->payload,
                               message->payload_size);
    if (!clone) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    clone->header = message->header;
    return clone;
}

AIRY_API const char *ipc_bus_proto_to_string(ipc_bus_proto_t proto)
{
    static const char *proto_strings[] = {"JSON-RPC", "MCP", "A2A", "OpenAI", "AUTO"};

    if (proto < 0 || proto > IPC_BUS_PROTO_AUTO)
        return "UNKNOWN";
    return proto_strings[proto];
}

AIRY_API ipc_bus_proto_t ipc_bus_proto_from_string(const char *str)
{
    if (!str)
        return IPC_BUS_PROTO_AUTO;

    if (strcasecmp(str, "JSON-RPC") == 0 || strcasecmp(str, "jsonrpc") == 0)
        return IPC_BUS_PROTO_JSON_RPC;
    if (strcasecmp(str, "MCP") == 0)
        return IPC_BUS_PROTO_MCP;
    if (strcasecmp(str, "A2A") == 0)
        return IPC_BUS_PROTO_A2A;
    if (strcasecmp(str, "OpenAI") == 0 || strcasecmp(str, "openai") == 0)
        return IPC_BUS_PROTO_OPENAI;

    return IPC_BUS_PROTO_AUTO;
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
    __builtin_memset(&bus->stats, 0, sizeof(ipc_bus_stats_t));
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
