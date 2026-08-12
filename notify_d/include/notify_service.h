/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file notify_service.h
 * @brief Notification service core (notify.* namespace).
 *
 * Carries the notify_d service core: channel subscription registry, ring
 * event queue, multi-protocol (WebSocket / SSE / Unix Socket) broadcast
 * engine and JSON-RPC method dispatch. The network layer (accept loop, WS
 * handshake, client lifecycle) is orchestrated by src/main.c.
 *
 */

#ifndef AIRY_RT_NOTIFY_SERVICE_H
#define AIRY_RT_NOTIFY_SERVICE_H

#include "platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOTIFY_D_MAX_BUFFER 65536
#define NOTIFY_D_MAX_PENDING 1024
#define NOTIFY_D_MAX_CLIENTS 128
#define NOTIFY_D_MAX_SUBSCRIPTIONS 512

typedef enum {
    NOTIFY_CLIENT_SOCKET,
    NOTIFY_CLIENT_WEBSOCKET,
    NOTIFY_CLIENT_SSE
} notify_client_type_t;

typedef struct {
    airy_sock_t fd;
    notify_client_type_t type;
    char *channel;
    char *client_id;
    uint64_t connected_at;
    uint64_t last_activity;
    uint64_t messages_sent;
    int active;
    char handshake_done;
} notify_client_t;

typedef struct {
    char *message;
    char *channel;
    char *event_type;
    uint64_t timestamp;
} notify_event_t;


typedef struct {
    char *client_id;
    char *channel;
    int active;
} notify_subscription_t;

typedef struct {
    airy_sock_t server_fd;
    airy_mtx_t lock;
    airy_thread_t event_thread;
    atomic_int running;
    atomic_int event_running;
    atomic_int force_stop;
    uint64_t start_time;
    uint64_t notified_count;
    uint64_t error_count;
    notify_client_t clients[NOTIFY_D_MAX_CLIENTS];
    size_t client_count;
    notify_event_t *pending[NOTIFY_D_MAX_PENDING];
    size_t pending_head;
    size_t pending_tail;
    size_t pending_count;
    notify_subscription_t subscriptions[NOTIFY_D_MAX_SUBSCRIPTIONS];
    size_t subscription_count;
    int tcp_port;
    char *socket_path;
} notify_d_service_t;


#define NOTIFY_D_METHOD_NOT_RPC 0
#define NOTIFY_D_METHOD_HANDLED 1
#define NOTIFY_D_METHOD_SHUTDOWN 2

int notify_d_service_init(notify_d_service_t *svc);
void notify_d_service_destroy(notify_d_service_t *svc);


int notify_d_subscribe(notify_d_service_t *svc, const char *channel, const char *client_id);
int notify_d_unsubscribe(notify_d_service_t *svc, const char *channel, const char *client_id);
int notify_d_has_subscription(notify_d_service_t *svc, const char *channel, const char *client_id);
size_t notify_d_subscription_count(notify_d_service_t *svc, const char *channel);


/* Note: notify_d_enqueue does not take the lock; the caller must hold it
 * (mutually exclusive with the consumer thread), or call it in a
 * single-threaded context (unit tests). */
int notify_d_enqueue(notify_d_service_t *svc, const char *msg, const char *channel,
                     const char *event_type);
int notify_d_broadcast_event(notify_d_service_t *svc, const notify_event_t *event);


size_t notify_d_active_client_count(notify_d_service_t *svc);


int notify_d_dispatch_jsonrpc(notify_d_service_t *svc, const char *request, char *response,
                              size_t response_size);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_NOTIFY_SERVICE_H */
