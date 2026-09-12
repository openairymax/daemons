/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file ipc_service_bus_internal.h
 * @brief Internal shared definitions of the ipc_service_bus static sources
 *        (not public API).
 *
 * Shared contract between the remaining two sources:
 *   - ipc_service_bus.c          bus lifecycle + channel basics + handler
 *                                registry + request transport + stats
 *   - ipc_service_bus_message.c  message create/free + protocol conversion
 *
 * 8.3.4 (0.1.15): the endpoint/event registry and the pending-request table
 * were removed with the never-delivering send/broadcast/notify family; the
 * bus only tracks an inflight-request counter now.
 *
 * This header is for the ipc_service_bus sources only; it must not be used
 * by other modules.
 *
 * @see agentrt/daemons/common/include/ipc_service_bus.h
 */

#ifndef AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_INTERNAL_H
#define AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_INTERNAL_H

#include "ipc_service_bus.h"

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_BUS_MAX_HANDLERS 16
#define IPC_BUS_MAX_INFLIGHT 256

typedef struct {
    ipc_bus_message_handler_t handler;
    void *user_data;
} message_handler_entry_t;

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
    ipc_bus_channel_internal_t *channels;
    uint32_t channel_count;
    ipc_bus_stats_t stats;
    bool running;
    airy_mtx_t mutex;
    uint32_t inflight_requests;
} ipc_service_bus_internal_t;

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_INTERNAL_H */
