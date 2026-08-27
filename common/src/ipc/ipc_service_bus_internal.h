/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file ipc_service_bus_internal.h
 * @brief Internal shared definitions of the ipc_service_bus static sources
 *        (not public API).
 *
 * After the Phase 2.3a split of ipc_service_bus.c into three sources, this
 * header carries the shared contract between the pieces:
 *   - ipc_service_bus.c          bus lifecycle + channel basics + send/
 *                                request/broadcast/notify + stats
 *   - ipc_service_bus_endpoint.c handler/endpoint registry + discovery +
 *                                load balancing
 *   - ipc_service_bus_message.c  message create/free/clone + header init +
 *                                protocol string conversion
 *
 * This header is for the ipc_service_bus sources only; it must not be used
 * by other modules.
 *
 * @see agentrt/daemons/common/include/ipc_service_bus.h
 */

#ifndef AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_INTERNAL_H
#define AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_INTERNAL_H

#include "ipc_service_bus.h"

#include "atomic_compat.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/**
 * @brief Fill an ipc_bus_message_header_t with the [SC] A-IPC standard
 *        header (Layout C v4) plus the service-bus semantic extension
 *        fields (msg_type/protocol/timestamp/source/target).
 * @note Implementation lives in ipc_service_bus_message.c; used by the
 *       message factory and by ipc_service_bus_notify() in the core file.
 */
void init_message_header(ipc_bus_message_header_t *header, ipc_bus_msg_type_t msg_type,
                         ipc_bus_proto_t protocol, const char *source, const char *target);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_INTERNAL_H */
