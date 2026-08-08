// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#ifndef DAEMON_EVENT_DRIVER_H
#define DAEMON_EVENT_DRIVER_H

#include "airy_event_loop.h"
#include "method_dispatcher.h"
#include "svc_common.h"
#include "thread_pool.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct daemon_event_driver daemon_event_driver_t;

typedef int (*daemon_on_client_cb)(void *service, airy_sock_t client_fd);

typedef void (*daemon_on_timer_cb)(void *service, uint64_t timer_id);

typedef struct daemon_event_config {
    int max_events;
    int thread_pool_min;
    int thread_pool_max;
    int thread_pool_queue_size;
    int health_check_interval_sec;
    bool use_jsonrpc;
    /* 若为 true 且配置了池 + on_client，则每个客户端请求被分派到线程池并发处理，
     * 事件循环线程不再被单个长请求（如阻塞等待交互审批的 execute）卡住。
     * 默认 false：保持原有同步逐请求处理语义（不影响其他 daemon）。 */
    bool concurrent_clients;
    daemon_on_client_cb on_client;
    daemon_on_timer_cb on_timer;
    void *service_ctx;
} daemon_event_config_t;

daemon_event_driver_t *daemon_event_driver_create(const daemon_event_config_t *config);

void daemon_event_driver_destroy(daemon_event_driver_t *driver);

int daemon_event_driver_add_server_fd(daemon_event_driver_t *driver, int fd);

int daemon_event_driver_add_fd(daemon_event_driver_t *driver, int fd, uint32_t events,
                               airy_event_callback_t cb, void *user_data);

uint64_t daemon_event_driver_add_timer(daemon_event_driver_t *driver, uint64_t interval_ms,
                                       airy_timer_callback_t cb, void *user_data);

int daemon_event_driver_cancel_timer(daemon_event_driver_t *driver, uint64_t timer_id);

int daemon_event_driver_run(daemon_event_driver_t *driver);

void daemon_event_driver_stop(daemon_event_driver_t *driver);

/**
 * @brief 异步安全停止事件驱动（可在信号处理器中安全调用）
 *
 * 仅触发底层事件循环的异步安全停止（原子置位 + eventfd 唤醒），
 * 不执行任何日志/锁操作，保持 async-signal-safe。
 */
void daemon_event_driver_stop_async(daemon_event_driver_t *driver);

airy_event_loop_t *daemon_event_driver_get_loop(daemon_event_driver_t *driver);

thread_pool_t *daemon_event_driver_get_pool(daemon_event_driver_t *driver);

method_dispatcher_t *daemon_event_driver_get_dispatcher(daemon_event_driver_t *driver);

#ifdef __cplusplus
}
#endif

#endif
