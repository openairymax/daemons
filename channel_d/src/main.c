/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.7.0 (L3 SSoT) 生成。
 * 机制层装配；策略层在 src/svc.c 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#include "platform.h"
#include "airy_rt.h"
#include "svc_channel_d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daemon_main.h"
#include "daemon_ipc_ops_bootstrap.h"

DAEMON_DECLARE_COMMON(channel_d, channel,
                      CHANNEL_D_SOCKET_UNIX, CHANNEL_D_SOCKET_WIN,
                      CHANNEL_D_TCP_PORT, CHANNEL_D_MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(channel_d)

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp,
                                     print_usage_channel_d);
    if (parse_rc > 0) return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_channel_d);

#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler_channel_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(channel_d);
#endif

    airy_logger_config_t log_cfg = {0};
    const char *dbg = getenv("AIRY_CHANNEL_D_DEBUG");
    log_cfg.level = (dbg && dbg[0] == '1') ? (log_level_t)LOG_LEVEL_DEBUG :
                     (log_level_t)LOG_LEVEL_WARN;
    airy_log_init(&log_cfg);
    atexit(log_cleanup);

    int core_ret = airy_init();
    if (core_ret == AIRY_SUCCESS)
        SVC_LOG_INFO("corekern core initialized (channel_d runs on corekern)");
    else
        SVC_LOG_WARN("corekern init failed (%d), degraded (badge=0)", core_ret);

    daemon_cupolas_init_pep("channel_d");
    daemon_ipc_ops_init("channel_d");

    if (svc_prepare(config_path) != 0) {
        SVC_LOG_ERROR("Service prepare failed");
        goto fail_svc;
    }

    daemon_endpoint_t ep;
    svc_endpoint(&ep, use_tcp);

    airy_sock_t server_fd = daemon_create_server_socket(
        ep.use_tcp, ep.tcp_port, ep.sock_unix, ep.sock_win);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        goto fail_svc;
    }

    daemon_event_config_t ev_config = {
        .max_events = 64, .thread_pool_min = 4,
        .thread_pool_max = 8, .thread_pool_queue_size = 256,
        .use_jsonrpc = true,
        .on_client = daemon_on_client_channel_d,
    };

    const char *sock_addr = ep.use_tcp ? ep.tcp_host : ep.sock_unix;
    int ret = daemon_init_event_driver(
        "channel_d", "channel", sock_addr,
        ep.use_tcp ? ep.tcp_port : 0, "channel,core",
        ep.use_tcp, &ev_config, &g_event_driver_channel_d, &g_bsd_channel_d,
        &g_bipc_channel_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_channel_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        goto fail_svc;
    }

    g_dispatcher_channel_d = daemon_event_driver_get_dispatcher(
        g_event_driver_channel_d);
    static const daemon_method_entry_t SVC_METHODS[] = {
        {"ping", m_ping},
        {"list", m_list},
        {"open", m_open},
        {"close", m_close},
        {"send", m_send},
        {"health", m_health},
        {"health_check", m_health_check},
        {"get_stats", m_get_stats},
        {"shutdown", on_shutdown_method_channel_d},
    };
    DAEMON_REGISTER_METHODS(g_dispatcher_channel_d, SVC_METHODS);
    SVC_LOG_INFO("Registered 9 RPC methods (channel.* namespace)");
    svc_attach(g_dispatcher_channel_d);

    if (daemon_event_driver_add_server_fd(g_event_driver_channel_d,
                                          (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        goto fail_driver;
    }

    if (svc_activate(g_event_driver_channel_d, g_bsd_channel_d) != 0) {
        SVC_LOG_ERROR("Service activate failed");
        goto fail_driver;
    }

    SVC_LOG_INFO("channel service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_channel_d);

    svc_teardown();
    daemon_cleanup_standard(g_bipc_channel_d, g_bsd_channel_d,
                            g_event_driver_channel_d, server_fd,
                            ep.sock_unix, svc_destroy,
                            &g_running_lock_channel_d);
    daemon_ipc_ops_cleanup();
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;

fail_driver:
    daemon_event_driver_destroy(g_event_driver_channel_d);
    airy_sock_close(server_fd);
fail_svc:
    svc_destroy();
    airy_mtx_destroy(&g_running_lock_channel_d);
    airy_sock_cleanup();
    return EXIT_FAILURE;
}
