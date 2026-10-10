/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file hook_internal.h
 * @brief notify_d hook 面（R7 并户：原 hook_d 独立守护进程吸收为第二监听
 *        面）的私有声明，与核心面 notify_d_internal.h 按域分离。
 * @details socket-only 面路由（全 daemons 零 L2 挂载实证）：hook 客户端
 *          连接 hook.sock（Windows TCP 走 SSoT AIRY_PORT_HOOK），12 个
 *          hook.* 方法载荷保真直迁；accept 循环按 face 打标，conn 线程按
 *          face 分派。承载 hook 端点常量 / 会话表类型与访问原语 /
 *          hook 面生命周期 / RPC 单入口返回码，仅限 notify_d 编译单元使用。
 *          从 notify_d_internal.h 拆出以降低枢纽头扇入（G13 load ≤ 5）。
 */

#ifndef AIRY_RT_DAEMON_NOTIFY_D_HOOK_INTERNAL_H
#define AIRY_RT_DAEMON_NOTIFY_D_HOOK_INTERNAL_H

#include "airy_defaults.h"
#include "platform.h"
#include "platform_paths.h"

#include "airy_abi.h"

AIRY_ABI_BEGIN

/* hook 面端点：socket-only 面路由（全 daemons 零 L2 挂载实证）。 */
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

AIRY_ABI_END

#endif /* AIRY_RT_DAEMON_NOTIFY_D_HOOK_INTERNAL_H */
