// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file ipc_service_bus_endpoint.c
 * @brief IPC service-bus implementation - handler/endpoint registry domain.
 *
 * Phase 2.3a split from ipc_service_bus.c: message/event handler
 * registration, endpoint registry (register/unregister), service
 * discovery, least-load endpoint selection and health updates.
 *
 * The public API surface (ipc_service_bus.h) is unchanged by this split.
 *
 * @see agentrt/daemons/common/src/ipc_service_bus.c (bus core domain)
 * @see agentrt/daemons/common/src/ipc_service_bus_internal.h
 */

#include "ipc_service_bus_internal.h"

#include "airy_memory.h"
#include "safe_string_utils.h"
#include "svc_logger.h"

#include <string.h>

#include "error.h"

#include "daemon_errors.h"

static int32_t find_endpoint_index(ipc_service_bus_internal_t *bus, const char *service_name)
{
    for (uint32_t i = 0; i < bus->endpoint_count; i++) {
        if (strcmp(bus->endpoints[i].service_name, service_name) == 0)
            return (int32_t)i;
    }
    return AIRY_ERR_NOT_FOUND;
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

    AIRY_LOG_INFO("Event handler registered for '%s' on bus '%s'", event_name, bus->name);
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
        AIRY_LOG_INFO("Endpoint '%s' updated on bus '%s'", endpoint->service_name, bus->name);
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

    AIRY_LOG_INFO("Endpoint '%s' registered on bus '%s' (endpoint=%s)", endpoint->service_name,
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
    AIRY_MEMSET(&bus->endpoints[bus->endpoint_count - 1], 0, sizeof(ipc_bus_endpoint_t));
    bus->endpoint_count--;
    bus->stats.active_endpoints = bus->endpoint_count;

    airy_mtx_unlock(&bus->mutex);

    AIRY_LOG_INFO("Endpoint '%s' unregistered from bus '%s'", service_name, bus->name);
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

    AIRY_LOG_DEBUG("Service discovery: found %u endpoints (name=%s, proto=%d)", count,
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
        AIRY_LOG_WARN("Endpoint '%s' became unhealthy on bus '%s'", service_name, bus->name);
    } else if (!was_healthy && healthy) {
        AIRY_LOG_INFO("Endpoint '%s' recovered on bus '%s'", service_name, bus->name);
    }

    return AIRY_SUCCESS;
}
