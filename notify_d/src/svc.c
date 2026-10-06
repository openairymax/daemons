/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief notify_d 生命周期策略域（gen5 手写层：服务单例 / 广播事件循环 /
 *        启停出口）。
 * @details 本户为异型户（codegen=false）：单端口四协议多路复用
 *        （JSON-RPC 短连接 / SSE / WebSocket / 裸消息），启停形态不
 *        走生成态六钩子。服务单例与停机标志由本域拥有；协议域
 *        （net.c）经 notify_d_internal.h 访问。stop 为唯一清理出口：
 *        force 模式的客户端与环形队列回收在持锁段内完成，与事件
 *        线程的 pending 消费互斥，消除标志复位与清理之间的竞态；
 *        destroy 不再隐式二次 stop，main.c 按显式顺序 stop → destroy
 *        装配。socket unlink 仅在 bind 成功后执行，防止误删其他
 *        实例的活动套接字文件。
 */

#include "airy_memory.h"
#include "error.h"
#include "notify_d_internal.h"
#include "daemon_main.h"
/* 事件线程经线程抽象双腿解析：无调度器实现时走 platform_process.h
 * 别名，airy_core 传播 AIRY_USE_SCHEDULER_THREAD_IMPL 时声明由
 * corekern task.h 提供（线程纳入调度器任务表记账）。两腿原型一致，
 * 调用点无需区分（gateway_d/service.c 同范式）。 */
#include "platform.h"
#include "platform_process.h"
#ifdef AIRY_USE_SCHEDULER_THREAD_IMPL
#include "task.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif

notify_d_service_t g_service = {0};
atomic_int g_shutdown = 0;

/* 本进程 bind 成功标记：stop 的 unlink 仅在其后执行（svc.c 私有） */
static int g_listened = 0;

/* airy_thread_create 入口统一为 airy_thread_func_t（void *(*)(void *)），
 * Windows 侧由 platform 层 airy_thread_start_routine 适配 __stdcall；
 * DWORD WINAPI 直传在 x86-32 触发 C2440（probe-3 实证）。 */
static void *notify_d_event_loop(void *arg)
{
    notify_d_service_t *svc = (notify_d_service_t *)arg;
    if (!svc)
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");

    while (svc->event_running) {
        airy_mtx_lock(&svc->lock);

        if (svc->pending_count > 0) {
            notify_event_t *event = svc->pending[svc->pending_head];
            svc->pending_head = (svc->pending_head + 1) % NOTIFY_D_MAX_PENDING;
            svc->pending_count--;

            airy_mtx_unlock(&svc->lock);

            notify_d_broadcast_event(svc, event);

            AIRY_FREE(event->message);
            AIRY_FREE(event->topic);
            AIRY_FREE(event->event_type);
            AIRY_FREE(event);
        } else {
            /* SSE 保活心跳：空闲期按 NOTIFY_D_SSE_PING_INTERVAL 周期发
             * 注释帧，防 SSE 客户端（gateway PEP epoch watch）空闲超时
             * 断线重连（实机 60s 节律断线 203 次的根因）。 */
            uint64_t now = (uint64_t)time(NULL);
            if (now - svc->last_sse_ping >= NOTIFY_D_SSE_PING_INTERVAL) {
                svc->last_sse_ping = now;
                notify_d_sse_heartbeat(svc);
            }
            airy_mtx_unlock(&svc->lock);

            for (int _w = 0; _w < 10 && svc->event_running; _w++) {
                airy_sleep_ms(100);
            }
        }
    }

    return NULL;
}

int notify_d_init(notify_d_service_t *svc, int port, const char *sock)
{
    if (!svc) {
        AIRY_ERROR(AIRY_EINVAL, "svc is NULL");
    }

    if (notify_d_service_init(svc) != AIRY_SUCCESS) {
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to init notify service core");
    }
    svc->tcp_port = port > 0 ? port : NOTIFY_D_DEFAULT_PORT;
    svc->socket_path = sock ? AIRY_STRDUP(sock) : AIRY_STRDUP(NOTIFY_D_DEFAULT_SOCKET);

    airy_sock_init();

    /* hook 面引导：SafetyGuard 注入 + airy_hook_init（失败非阻断降级，
     * health 报不健康，服务继续受理）+ 内置生产 handler 注册 */
    hook_svc_prepare();

    SVC_LOG_INFO("notify_d: init complete (max_clients=%d)", NOTIFY_D_MAX_CLIENTS);
    return AIRY_SUCCESS;
}

int notify_d_start(notify_d_service_t *svc)
{
    if (!svc) {
        AIRY_ERROR(AIRY_EINVAL, "svc is NULL");
    }

#ifndef _WIN32
    svc->server_fd = airy_sock_create_unix_server(svc->socket_path);
    if (svc->server_fd < 0) {
        SVC_LOG_ERROR("notify_d: failed to create socket at %s", svc->socket_path);
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to create unix socket");
    }
#else
    svc->server_fd = airy_sock_create_tcp_server("127.0.0.1", (uint16_t)svc->tcp_port);
    if (svc->server_fd < 0) {
        SVC_LOG_ERROR("notify_d: failed to create TCP server");
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to create TCP server");
    }
#endif

    g_listened = 1;
    svc->running = 1;
    svc->event_running = 1;
    svc->force_stop = 0;

    /* hook 面第二 listener：失败 fail-fast（进程退出由 main 处理），
     * 避免半启动态（notify 面在、hook 面缺失）不可诊断 */
    if (hook_svc_listen_up() != AIRY_SUCCESS)
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to start hook face listener");

    airy_thread_create(&svc->event_thread, notify_d_event_loop, svc);

    SVC_LOG_INFO("notify_d: service started (event_loop=active)");
    return AIRY_SUCCESS;
}

int notify_d_stop(notify_d_service_t *svc, int force)
{
    if (!svc) {
        AIRY_ERROR(AIRY_EINVAL, "svc is NULL");
    }

    if (!force) {
        airy_mtx_lock(&svc->lock);
        svc->running = 0;
        svc->event_running = 0;
        airy_mtx_unlock(&svc->lock);

        airy_thread_join(svc->event_thread, NULL);

        airy_mtx_lock(&svc->lock);
    } else {
        airy_mtx_lock(&svc->lock);
        svc->running = 0;
        svc->event_running = 0;
        svc->force_stop = 1;
    }

    for (size_t i = 0; i < svc->client_count; i++) {
        if (force && svc->clients[i].active && svc->clients[i].fd != AIRY_INVALID_SOCKET) {
            airy_sock_close(svc->clients[i].fd);
            svc->clients[i].fd = AIRY_INVALID_SOCKET;
            svc->clients[i].active = 0;
        }
    }

    if (force)
        notify_d_drain(svc);

    airy_mtx_unlock(&svc->lock);

    if (svc->server_fd != AIRY_INVALID_SOCKET) {
        airy_sock_close(svc->server_fd);
        svc->server_fd = AIRY_INVALID_SOCKET;
    }
    hook_svc_listen_down();

#ifndef _WIN32
    if (force && g_listened) {
        unlink(svc->socket_path);
        g_listened = 0;
    }
#endif

    SVC_LOG_INFO("notify_d: service stopped (force=%d, pending=%zu, clients=%zu)", force,
                 svc->pending_count, svc->client_count);
    return AIRY_SUCCESS;
}

int notify_d_destroy(notify_d_service_t *svc)
{
    if (!svc) {
        AIRY_ERROR(AIRY_EINVAL, "svc is NULL");
    }

    /* hook 面收尾先于服务核心：unregister 内置 handler → 唯一释放路径
     * airy_hook_shutdown → SafetyGuard 摘除 → 会话锁销毁（幂等） */
    hook_svc_destroy();
    notify_d_service_destroy(svc);
    airy_sock_cleanup();
    AIRY_FREE(svc->socket_path);
    AIRY_MEMSET(svc, 0, sizeof(*svc));
    SVC_LOG_INFO("notify_d: service destroyed");
    return AIRY_SUCCESS;
}

int notify_d_healthcheck(notify_d_service_t *svc)
{
    if (!svc)
        return 0;

    airy_mtx_lock(&svc->lock);
    int healthy = svc->running && svc->event_running ? 1 : 0;
    size_t active_clients = 0;
    for (size_t i = 0; i < svc->client_count; i++) {
        if (svc->clients[i].active)
            active_clients++;
    }
    size_t pending = svc->pending_count;
    size_t error_count = svc->error_count;
    size_t notified_count = svc->notified_count;
    airy_mtx_unlock(&svc->lock);

    if (pending >= NOTIFY_D_MAX_PENDING)
        healthy = 0;
    if (error_count > notified_count / 2 && notified_count > 10)
        healthy = 0;

    (void)active_clients;
    return healthy;
}
