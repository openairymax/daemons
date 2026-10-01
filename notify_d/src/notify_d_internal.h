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

#include "airy_defaults.h"
#include "atomic_compat.h"
#include "notify_service.h"
#include "platform_paths.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* ---- hook 面（R7 并户：原 hook_d 独立守护进程吸收为第二监听面）----
 * socket-only 面路由（全 daemons 零 L2 挂载实证）：hook 客户端连接
 * hook.sock（Windows TCP 走 SSoT AIRY_PORT_HOOK），12 个 hook.* 方法载荷
 * 保真直迁；accept 循环按 face 打标，conn 线程按 face 分派。 */
#define HOOK_D_SOCKET_UNIX airy_runtime_dir_socket("hook.sock")
#define HOOK_D_TCP_PORT AIRY_PORT_HOOK
#define AIRY_HOOK_MAX_SESSIONS 32
#define AIRY_HOOK_CTX_LEN 4096

typedef struct {
    char session_id[64];
    bool active;
    uint64_t started_ns;
    char decision[16];
    size_t hook_count;
    char context[AIRY_HOOK_CTX_LEN]; /* 注入的会话上下文（session.start 的 input） */
    size_t context_len;
} hook_session_entry_t;

/* Hook 系统引导标志与启动时刻（hook_svc.c 拥有，hook_rpc.c 只读） */
extern int g_hook_registry_ready;
extern uint64_t g_hook_start_time;

/* P1-5 会话级上下文注入通道存储（hook_svc.c 初始化/销毁锁，
 * handler 经 hook_session_find / hook_session_upsert 访问） */
extern hook_session_entry_t g_hook_sessions[AIRY_HOOK_MAX_SESSIONS];
extern airy_mtx_t g_hook_sessions_lock;

/* 会话表访问原语（hook_svc.c 实现；调用方负责持锁） */
hook_session_entry_t *hook_session_find(const char *session_id);
hook_session_entry_t *hook_session_upsert(const char *session_id);

/* hook 面生命周期（hook_svc.c 实现，svc.c 装配调用；全部幂等） */
int hook_svc_prepare(void);
void hook_svc_destroy(void);
int hook_svc_listen_up(void);
void hook_svc_listen_down(void);
airy_sock_t hook_svc_listen_fd(void);

/* hook 面单连接受理（hook_svc.c 实现，net.c conn 线程按 face 调用；
 * read_request 失败回 -32600 后关连接，单请求单回包） */
void hook_svc_serve_conn(airy_sock_t fd);

/* hook 面 RPC 方法域单入口返回码 */
#define HOOK_RPC_HANDLED 0
#define HOOK_RPC_SHUTDOWN 1

/* hook 面 RPC 单入口（hook_rpc.c 实现）：解析 + 校验 + 13 方法分派，
 * 回包串交 *out（调用方发送后释放）；shutdown 返回 HOOK_RPC_SHUTDOWN */
int hook_rpc_handle_json(const char *req_text, size_t req_len, char **out);

/* accept 循环面标记与每连接参数（main.c 装配，net.c 消费） */
#define NOTIFY_FACE_NOTIFY 0
#define NOTIFY_FACE_HOOK 1

typedef struct {
    airy_sock_t fd;
    int face;
} notify_conn_arg_t;

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_NOTIFY_D_INTERNAL_H */
