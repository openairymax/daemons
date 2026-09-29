/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file hook_svc.c
 * @brief notify_d hook 面生命周期策略域（R7 并户：原 hook_d 独立守护
 *        进程的 svc.c 域吸收为第二监听面）。
 * @details hook 系统核心（registry/executor/interceptor/timeout/
 *          handlers）在 airy_coreloop_hooks 接口库，本域仅负责装配：
 *          SafetyGuard 实现经 airy_safety_ops_t 注入（供 atoms 调用点
 *          在 PRE_TOOL/PRE_EXEC 拦截链使用）、airy_hook_init 引导
 *          （失败非阻断降级，health 报不健康，服务继续受理）、P1-5
 *          会话级上下文注入通道存储。第二 listener 随 notify_d 服务
 *          生命周期同启同停：prepare/destroy 由 notify_d_init/destroy
 *          装配，listen_up/listen_down 由 notify_d_start/stop 装配。
 */

#include "airy_memory.h"
#include "jsonrpc_helpers.h"
#include "notify_d_internal.h"
#include "airy_hook.h"
#include "airy_safety_ops.h"
#include "hook_builtin_handlers.h"
#include "safety_guard.h"
#include "svc_logger.h"

#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif

int g_hook_registry_ready = 0;
uint64_t g_hook_start_time = 0;

hook_session_entry_t g_hook_sessions[AIRY_HOOK_MAX_SESSIONS];
airy_mtx_t g_hook_sessions_lock;

/* 本域装配完成标记与第二 listener 状态（文件私有，保证幂等） */
static int g_hook_prepared = 0;
static airy_sock_t g_hook_listen_fd = AIRY_INVALID_SOCKET;
static int g_hook_listened = 0;

hook_session_entry_t *hook_session_find(const char *session_id)
{
    for (size_t i = 0; i < AIRY_HOOK_MAX_SESSIONS; i++) {
        if (g_hook_sessions[i].active &&
            strcmp(g_hook_sessions[i].session_id, session_id) == 0)
            return &g_hook_sessions[i];
    }
    return NULL;
}

hook_session_entry_t *hook_session_upsert(const char *session_id)
{
    hook_session_entry_t *e = hook_session_find(session_id);
    if (e)
        return e;
    size_t slot = AIRY_HOOK_MAX_SESSIONS;
    size_t oldest = 0;
    for (size_t i = 0; i < AIRY_HOOK_MAX_SESSIONS; i++) {
        if (!g_hook_sessions[i].active) {
            slot = i;
            break;
        }
        if (g_hook_sessions[i].started_ns < g_hook_sessions[oldest].started_ns)
            oldest = i;
    }
    if (slot == AIRY_HOOK_MAX_SESSIONS)
        slot = oldest; /* 全满：覆盖最旧会话 */
    AIRY_MEMSET(&g_hook_sessions[slot], 0, sizeof(g_hook_sessions[slot]));
    AIRY_STRNCPY_TERM(g_hook_sessions[slot].session_id, session_id,
                      sizeof(g_hook_sessions[slot].session_id));
    g_hook_sessions[slot].active = true;
    g_hook_sessions[slot].started_ns = (uint64_t)time(NULL) * 1000000000ull;
    return &g_hook_sessions[slot];
}

static safety_guard_context_t *hook_safety_create(void)
{
    return safety_guard_create();
}

static safety_decision_t hook_safety_check_chain(safety_guard_context_t *ctx,
                                                 const safety_event_t *event,
                                                 safety_result_t **results, size_t *result_count)
{
    return safety_guard_check_chain(ctx, event, results, result_count);
}

static void hook_safety_destroy(safety_guard_context_t *ctx)
{
    safety_guard_destroy(ctx);
}

static const airy_safety_ops_t g_hook_safety_ops = {
    .create = hook_safety_create,
    .check_chain = hook_safety_check_chain,
    .destroy = hook_safety_destroy,
};

int hook_svc_prepare(void)
{
    if (g_hook_prepared)
        return AIRY_SUCCESS;

    airy_mtx_init(&g_hook_sessions_lock);
    are_ops_set_safety(&g_hook_safety_ops);
    g_hook_start_time = (uint64_t)time(NULL);

    if (airy_hook_init() == 0) {
        g_hook_registry_ready = 1;
        /* 内置生产 handler（audit/metrics/trace）统一注册，
         * status/list 由此返回真实加载的 hook 模块信息 */
        airy_hook_register_builtin_handlers();
        SVC_LOG_INFO("hook face: hook system initialized (registry + timeout manager)");
    } else {
        SVC_LOG_ERROR("hook face: hook system init failed (degraded)");
    }
    g_hook_prepared = 1;
    return AIRY_SUCCESS;
}

void hook_svc_destroy(void)
{
    if (!g_hook_prepared)
        return;
    if (g_hook_registry_ready) {
        airy_hook_unregister_builtin_handlers();
        /* airy_hook_init 是注册表 + 超时管理器的唯一生命周期入口；
         * 直调 hook_registry_destroy 会跳过超时管理器并绕过共享引用
         * 计数，保持唯一释放路径 */
        airy_hook_shutdown();
        g_hook_registry_ready = 0;
    }
    are_ops_set_safety(NULL);
    airy_mtx_destroy(&g_hook_sessions_lock);
    g_hook_prepared = 0;
}

int hook_svc_listen_up(void)
{
    if (g_hook_listened)
        return AIRY_SUCCESS;

#ifndef _WIN32
    g_hook_listen_fd = airy_sock_create_unix_server(HOOK_D_SOCKET_UNIX);
    if (g_hook_listen_fd < 0) {
        SVC_LOG_ERROR("hook face: failed to create socket at %s", HOOK_D_SOCKET_UNIX);
        return AIRY_ERR_UNKNOWN;
    }
#else
    g_hook_listen_fd = airy_sock_create_tcp_server("127.0.0.1", HOOK_D_TCP_PORT);
    if (g_hook_listen_fd < 0) {
        SVC_LOG_ERROR("hook face: failed to create TCP server on port %d", HOOK_D_TCP_PORT);
        return AIRY_ERR_UNKNOWN;
    }
#endif

    g_hook_listened = 1;
    SVC_LOG_INFO("hook face: listening");
    return AIRY_SUCCESS;
}

void hook_svc_listen_down(void)
{
    if (g_hook_listen_fd != AIRY_INVALID_SOCKET) {
        airy_sock_close(g_hook_listen_fd);
        g_hook_listen_fd = AIRY_INVALID_SOCKET;
    }
#ifndef _WIN32
    if (g_hook_listened)
        unlink(HOOK_D_SOCKET_UNIX);
#endif
    g_hook_listened = 0;
}

airy_sock_t hook_svc_listen_fd(void)
{
    return g_hook_listen_fd;
}

void hook_svc_serve_conn(airy_sock_t fd)
{
    /* 镜像 daemon_handle_client 语义：read_request 循环探测 JSON 完整性
     * （大请求不截断），失败回 -32600 后关连接；单请求单回包。 */
    size_t req_len = 0;
    const char *req_err = NULL;
    char *req_text = airy_daemon_read_request(fd, &req_len, &req_err);
    if (!req_text) {
        char *err = jsonrpc_build_error(JSONRPC_INVALID_REQUEST,
                                        req_err ? req_err : "Request read failed", -1);
        if (err) {
            airy_sock_send(fd, err, strlen(err));
            AIRY_FREE(err);
        }
        airy_sock_close(fd);
        return;
    }

    char *out = NULL;
    int rc = hook_rpc_handle_json(req_text, req_len, &out);
    if (rc == HOOK_RPC_SHUTDOWN)
        atomic_store_explicit(&g_shutdown, 1, memory_order_seq_cst);
    if (out) {
        airy_sock_send(fd, out, strlen(out));
        AIRY_FREE(out);
    }
    airy_sock_close(fd);
}
