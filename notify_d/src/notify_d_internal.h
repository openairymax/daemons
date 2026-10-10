/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file notify_d_internal.h
 * @brief notify_d 守护进程核心面共享的内部声明
 *        （main.c / svc.c / net.c / nf_boot.c / hook_svc.c）。
 * @details notify_d 为异型户（manifest codegen=false）：单端口上 JSON-RPC
 *          短连接 / SSE 长连接 / WebSocket 长连接 / 裸消息投递四协议
 *          多路复用，每连接一线程，自建 WS 握手与环形队列广播引擎，
 *          与生成态 event_driver + SVC_METHODS 静态表不可兼容。协议
 *          核心（订阅注册表 / 事件队列 / 广播 / JSON-RPC 分派）在
 *          notify_service.c；本头仅承载 daemon 私有的端点常量、进程
 *          级状态量、生命周期/协议/启停策略域声明，以及 accept 循环
 *          面标记与每连接参数。仅限 notify_d 编译单元使用。
 * @note  hook 面（R7 并户）声明已按域分离至 hook_internal.h，
 *        以降低枢纽头扇入（G13 load ≤ 5）。
 */

#ifndef AIRY_RT_DAEMON_NOTIFY_D_INTERNAL_H
#define AIRY_RT_DAEMON_NOTIFY_D_INTERNAL_H

#include "airy_defaults.h"
#include "atomic_compat.h"
#include "notify_service.h"
#include "platform_paths.h"

#include "airy_abi.h"

AIRY_ABI_BEGIN

/* 端点常量：Windows 端固定 TCP 回环（无命名管）是协议面必然——
 * 浏览器 EventSource / WebSocket 无法使用 Windows 命名管道，
 * 与常规户「命名管优先 TCP 回退」形态不同，非平台屏蔽债务。
 * 端口值为 SSoT 引用，唯一权威定义见 commons/include/airy_defaults.h。 */
#define NOTIFY_D_DEFAULT_PORT AIRY_PORT_NOTIFY_D
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

/* 启停策略域（nf_boot.c 实现，main.c 装配调用） */
void nf_sig_install(void);
void nf_conn_admit(airy_sock_t hook_fd);
void nf_sd_announce(notify_d_service_t *svc);
void nf_teardown(void);

/* accept 循环面标记与每连接参数（main.c 装配，net.c 消费） */
#define NOTIFY_FACE_NOTIFY 0
#define NOTIFY_FACE_HOOK 1

typedef struct {
    airy_sock_t fd;
    int face;
} notify_conn_arg_t;

AIRY_ABI_END

#endif /* AIRY_RT_DAEMON_NOTIFY_D_INTERNAL_H */
