/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file nf_boot.c
 * @brief notify_d 启停策略域（gen5 异型户，codegen=false）：信号装配、
 *        双面连接受理与停机清理。
 * @details 五件套形态：main.c 只留装配骨架与主循环，策略块归本域。
 *        承接自原 main.c：信号 handler（原子停机旗标 + async-signal-
 *        safe stderr 追踪）、notify.sock/hook.sock 双面等待受理（POSIX
 *        poll 双 listener / Windows 顺序短超时试探）、过载拒绝与每连接
 *        一线程派发、SD 公告与 teardown。行为与原装配面逐行同构，日志
 *        文案原文保留。
 */

#include "atomic_compat.h"
#include "airy_memory.h"
#include "airy_rt.h"
#include "daemon_bootstrap_sd.h"
#include "daemon_ipc_ops_bootstrap.h"
#include "daemon_platform_ext.h"
#include "daemon_security_dome.h"
#include "logging.h"
#include "notify_d_internal.h"

#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

static daemon_bootstrap_sd_t *s_bsd = NULL;

static void nf_on_signal(int sig)
{
    (void)sig;

    atomic_store_explicit(&g_shutdown, 1, memory_order_seq_cst);
#ifndef _WIN32
    {
        static const char sig_msg[] =
            "[SIG] shutdown signal received, initiating graceful shutdown\n";
        ssize_t written = write(STDERR_FILENO, sig_msg, sizeof(sig_msg) - 1);
        (void)written; /* best-effort diagnostics inside a signal handler */
    }
#endif
}

void nf_sig_install(void)
{
#ifndef _WIN32
    signal(SIGINT, nf_on_signal);
    signal(SIGTERM, nf_on_signal);
    signal(SIGPIPE, SIG_IGN);
#endif
}

void nf_conn_admit(airy_sock_t hook_fd)
{
    int face;
    airy_sock_t client;

#ifndef _WIN32
    /* 双面等待：poll 两 listener（notify.sock + hook.sock），1000ms
     * 步进与停机检查节奏一致；命中后 accept 非阻塞取连接 */
    struct pollfd pfds[2];
    pfds[0].fd = (int)g_service.server_fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = (int)hook_fd;
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;
    if (poll(pfds, 2, 1000) <= 0)
        return;
    face = (pfds[1].revents & POLLIN) ? NOTIFY_FACE_HOOK : NOTIFY_FACE_NOTIFY;
    client = airy_sock_accept(
        face == NOTIFY_FACE_HOOK ? hook_fd : g_service.server_fd, 0);
#else
    /* Windows 无跨族双 fd 等待：顺序短超时试探两面，50ms 步进
     * 保持停机响应（timeout=0 在 select 路径是无限阻塞，禁用） */
    client = airy_sock_accept(g_service.server_fd, 50);
    if (client != AIRY_INVALID_SOCKET) {
        face = NOTIFY_FACE_NOTIFY;
    } else {
        face = NOTIFY_FACE_HOOK;
        client = airy_sock_accept(hook_fd, 50);
    }
#endif
    if (client == AIRY_INVALID_SOCKET)
        return;
    if (atomic_load_explicit(&g_conns, memory_order_relaxed) >= NOTIFY_D_MAX_CONN) {
        airy_sock_close(client); /* 过载：拒绝新连接 */
        return;
    }
    atomic_fetch_add_explicit(&g_conns, 1, memory_order_relaxed);
    notify_conn_arg_t *carg = AIRY_MALLOC(sizeof(*carg));
    if (!carg) {
        airy_sock_close(client);
        atomic_fetch_sub_explicit(&g_conns, 1, memory_order_relaxed);
        return;
    }
    carg->fd = client;
    carg->face = face;
    airy_thread_t th;
    /* fire-and-forget conn thread: platform primitives on purpose.
     * Under AIRY_USE_SCHEDULER_THREAD_IMPL the scheduler owns
     * airy_thread_create/join (join-oriented, no detach API); a
     * per-conn blocking handler must not occupy the task table. */
    if (airy_platform_thread_create(&th, notify_d_conn_thread, carg) == 0)
        airy_platform_thread_detach(th);
    else {
        AIRY_FREE(carg);
        atomic_fetch_sub_explicit(&g_conns, 1, memory_order_relaxed);
        airy_sock_close(client);
    }
}

void nf_sd_announce(notify_d_service_t *svc)
{
    s_bsd = daemon_bootstrap_sd_start(
        "notify_d", "notify", svc->socket_path, 0, "notify,core", 0);
}

void nf_teardown(void)
{
    daemon_bootstrap_sd_stop(s_bsd);
    notify_d_stop(&g_service, g_shutdown ? 1 : 0);
    notify_d_destroy(&g_service);
    daemon_ipc_ops_cleanup();
    daemon_dome_cleanup();
    log_cleanup();
}
