/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file notify_d_internal.h
 * @brief notify_d 守护进程各编译单元共享的内部声明
 *        （svc.c / net.c / main.c）。
 * @details notify_d 为异型户（manifest codegen=false）：单端口上 JSON-RPC
 *          短连接 / SSE 长连接 / WebSocket 长连接 / 裸消息投递四协议
 *          多路复用，每连接一线程，自建 WS 握手与环形队列广播引擎，
 *          与生成态 event_driver + SVC_METHODS 静态表不可兼容。协议
 *          核心（订阅注册表 / 事件队列 / 广播 / JSON-RPC 分派）在
 *          notify_service.c；本头仅承载 daemon 私有的端点常量、进程
 *          级状态量与跨域入口声明。仅限 notify_d 编译单元使用。
 */

#ifndef AIRY_RT_DAEMON_NOTIFY_D_INTERNAL_H
#define AIRY_RT_DAEMON_NOTIFY_D_INTERNAL_H

#include "atomic_compat.h"
#include "notify_service.h"
#include "platform_paths.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 端点常量：Windows 端固定 TCP 回环（无命名管）是协议面必然——
 * 浏览器 EventSource / WebSocket 无法使用 Windows 命名管道，
 * 与常规户「命名管优先 TCP 回退」形态不同，非平台屏蔽债务。 */
#define NOTIFY_D_DEFAULT_PORT 8084
#define NOTIFY_D_DEFAULT_SOCKET airy_runtime_dir_socket("notify.sock")

/* 并发连接上限：每连接一线程，无上限时恶意连接风暴可耗尽线程资源
 * （工业场景 DoS 防护）。超过上限的新连接直接关闭。 */
#define NOTIFY_D_MAX_CONN 128

/* 进程级状态：svc.c 拥有服务单例与停机标志，net.c 拥有在途连接计数 */
extern notify_d_service_t g_service;
extern atomic_int g_shutdown;
extern atomic_int g_conns;

/* 生命周期域（svc.c 实现，main.c 装配调用） */
int notify_d_init(notify_d_service_t *svc, int port, const char *sock);
int notify_d_start(notify_d_service_t *svc);
int notify_d_stop(notify_d_service_t *svc, int force);
int notify_d_destroy(notify_d_service_t *svc);
int notify_d_healthcheck(notify_d_service_t *svc);

/* 协议域（net.c 实现，main.c accept 循环派发） */
void *notify_d_conn_thread(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_NOTIFY_D_INTERNAL_H */
