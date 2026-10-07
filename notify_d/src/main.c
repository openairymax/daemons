/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file main.c
 * @brief notify_d 装配域（gen5 异型户，codegen=false）：装配骨架与
 *        双面 accept 主循环。
 * @details 单端口四协议多路复用（JSON-RPC / SSE / WebSocket / 裸消息），
 *        R7 并户后装配双监听面：notify.sock（Windows TCP 走 SSoT
 *        AIRY_PORT_NOTIFY_D）+ hook.sock（Windows TCP 走 SSoT
 *        AIRY_PORT_HOOK）。生命周期策略在 svc.c，启停策略域在
 *        nf_boot.c（信号/受理/SD/teardown），协议嗅探与握手在 net.c，
 *        订阅/广播/分派核心在 notify_service.c。停机出口唯一：main
 *        显式 stop(force) 后 destroy，destroy 不再隐式二次 stop。
 */

#include "airy_rt.h"
#include "daemon_cupolas_bootstrap.h"
#include "daemon_ipc_ops_bootstrap.h"
#include "error.h"
#include "logging.h"
#include "notify_d_internal.h"
#include "svc_logger.h"

#include <stdlib.h>

int main(void)
{
    nf_sig_install();
    log_init(NULL);
    atexit(log_cleanup);

    /* WS-8 stage 4 (8.4.1): corekern core first link of the boot chain;
     * airy_init() is idempotent, failure degrades to platform fallbacks
     * (non-fatal, badge=0). */
    {
        int core_ret = airy_init();
        if (core_ret == AIRY_SUCCESS) {
            SVC_LOG_INFO("corekern core initialized (notify_d runs on corekern)");
        } else {
            SVC_LOG_WARN("corekern init failed (%d) - running degraded (badge=0)", core_ret);
        }
    }

    daemon_cupolas_init_pep("notify_d");
    /* IPC/RPC/SD ops table for atoms call sites; init failure is
     * non-fatal: atoms callers degrade gracefully. */
    daemon_ipc_ops_init("notify_d");

    if (notify_d_init(&g_service, NOTIFY_D_DEFAULT_PORT, NOTIFY_D_DEFAULT_SOCKET) != AIRY_SUCCESS)
        return EXIT_FAILURE;
    if (notify_d_start(&g_service) != AIRY_SUCCESS) {
        notify_d_stop(&g_service, 1);
        notify_d_destroy(&g_service);
        return EXIT_FAILURE;
    }

    nf_sd_announce(&g_service);

    airy_sock_t hook_fd = hook_svc_listen_fd();
    while (!g_shutdown && g_service.running)
        nf_conn_admit(hook_fd);

    nf_teardown();
    return 0;
}
