/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file main.c
 * @brief notify_d 装配域（gen5 异型户，codegen=false）：boot 序列与
 *        accept 循环。
 * @details 本户单端口四协议多路复用（JSON-RPC / SSE / WebSocket /
 *        裸消息），每连接一线程，不使用 DAEMON_DECLARE_COMMON 生成
 *        样板。生命周期策略在 svc.c，协议嗅探与握手在 net.c，
 *        订阅/广播/分派核心在 notify_service.c。停机出口唯一：
 *        main 显式 stop(force) 后 destroy，destroy 不再隐式二次
 *        stop。
 */

#include "airy_memory.h"
#include "airy_rt.h"
#include "error.h"
#include "daemon_main.h"
#include "daemon_ipc_ops_bootstrap.h"
#include "notify_d_internal.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#endif

static void notify_d_signal_handler(int sig)
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

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#ifndef _WIN32
    signal(SIGINT, notify_d_signal_handler);
    signal(SIGTERM, notify_d_signal_handler);
    signal(SIGPIPE, SIG_IGN);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    /* WS-8 stage 4 (8.4.1): bring up the corekern core (mem/oom/task/ipc/
     * eventloop/persist) as the first link of the daemon boot chain, before
     * the daemon's own subsystems. airy_init() is idempotent; if it fails
     * the daemon still runs on the platform fallbacks (DSL degradation,
     * non-fatal, badge=0). */
    {
        int core_ret = airy_init();
        if (core_ret == AIRY_SUCCESS) {
            SVC_LOG_INFO("corekern core initialized (notify_d runs on corekern)");
        } else {
            SVC_LOG_WARN("corekern init failed (%d) - running degraded (badge=0)", core_ret);
        }
    }

    daemon_cupolas_init_pep("notify_d");

    /* Publish the IPC/RPC/SD ops table to atoms call sites so they dispatch
     * without linking daemons symbols. Init failure is non-fatal: atoms
     * callers degrade gracefully. */
    daemon_ipc_ops_init("notify_d");

    if (notify_d_init(&g_service, NOTIFY_D_DEFAULT_PORT, NOTIFY_D_DEFAULT_SOCKET) != AIRY_SUCCESS)
        return EXIT_FAILURE;
    if (notify_d_start(&g_service) != AIRY_SUCCESS) {
        notify_d_stop(&g_service, 1);
        notify_d_destroy(&g_service);
        return EXIT_FAILURE;
    }

    daemon_bootstrap_sd_t *bsd =
        daemon_bootstrap_sd_start("notify_d", "notify", g_service.socket_path, 0, "notify,core", 0);
    daemon_bootstrap_ipc_t *bipc = daemon_bootstrap_ipc_start(
        "notify_d", "notify", g_service.socket_path, 0, IPC_BUS_PROTO_JSON_RPC);

    while (!g_shutdown && g_service.running) {
        airy_sock_t client = airy_sock_accept(g_service.server_fd, 1000);
        if (client != AIRY_INVALID_SOCKET) {
            if (atomic_load_explicit(&g_conns, memory_order_relaxed) >= NOTIFY_D_MAX_CONN) {
                airy_sock_close(client); /* 过载：拒绝新连接 */
                continue;
            }
            atomic_fetch_add_explicit(&g_conns, 1, memory_order_relaxed);
            airy_thread_t th;
            /* fire-and-forget conn thread: platform primitives on purpose.
             * Under AIRY_USE_SCHEDULER_THREAD_IMPL the scheduler owns
             * airy_thread_create/join (join-oriented, no detach API); a
             * per-conn blocking handler must not occupy the task table. */
            if (airy_platform_thread_create(&th, notify_d_conn_thread,
                                            (void *)(intptr_t)client) == 0)
                airy_platform_thread_detach(th);
            else {
                atomic_fetch_sub_explicit(&g_conns, 1, memory_order_relaxed);
                airy_sock_close(client);
            }
        }
    }

    daemon_bootstrap_ipc_stop(bipc);
    daemon_bootstrap_sd_stop(bsd);
    notify_d_stop(&g_service, g_shutdown ? 1 : 0);
    notify_d_destroy(&g_service);
    daemon_ipc_ops_cleanup();
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
