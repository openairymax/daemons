// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file notify_service.h
 * @brief 通知服务核心（notify.* 命名空间）
 *
 * 承载 notify_d 的服务核心逻辑：频道订阅注册表、环形事件队列、
 * 多协议（WebSocket / SSE / Unix Socket）广播引擎与 JSON-RPC 方法分发。
 * 网络层（accept 循环、WS 握手、客户端生命周期）由 src/main.c 编排。
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
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
    char *channel;    /* 客户端连接的频道（X-Channel 头，单频道） */
    char *client_id;  /* 客户端标识（X-Client-Id 头，用于订阅注册表投递匹配） */
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

/* 频道订阅注册表条目：(channel, client_id) 逻辑订阅 */
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

/* JSON-RPC 分发返回码 */
#define NOTIFY_D_METHOD_NOT_RPC 0   /* 非 JSON-RPC 或未知方法：交由原协议路径处理 */
#define NOTIFY_D_METHOD_HANDLED 1   /* 已处理，response 填充合法 JSON-RPC 响应 */
#define NOTIFY_D_METHOD_SHUTDOWN 2  /* 已处理 shutdown，调用方应触发优雅关闭 */

/* ---------- 服务核心生命周期（网络层由 main.c 编排） ---------- */
int notify_d_service_init(notify_d_service_t *svc);
void notify_d_service_destroy(notify_d_service_t *svc);

/* ---------- 频道订阅注册表 ---------- */
int notify_d_subscribe(notify_d_service_t *svc, const char *channel, const char *client_id);
int notify_d_unsubscribe(notify_d_service_t *svc, const char *channel, const char *client_id);
int notify_d_has_subscription(notify_d_service_t *svc, const char *channel, const char *client_id);
size_t notify_d_subscription_count(notify_d_service_t *svc, const char *channel);

/* ---------- 事件队列与广播 ---------- */
/* 注意：notify_d_enqueue 不持有锁，调用方须保证持锁（与消费线程互斥），
 * 或在单线程上下文（单元测试）中调用。 */
int notify_d_enqueue(notify_d_service_t *svc, const char *msg, const char *channel,
                     const char *event_type);
int notify_d_broadcast_event(notify_d_service_t *svc, const notify_event_t *event);

/* ---------- 查询辅助 ---------- */
size_t notify_d_active_client_count(notify_d_service_t *svc);

/* ---------- JSON-RPC 方法分发（L2 命名空间方法，方法名不带前缀） ---------- */
int notify_d_dispatch_jsonrpc(notify_d_service_t *svc, const char *request,
                              char *response, size_t response_size);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_NOTIFY_SERVICE_H */
