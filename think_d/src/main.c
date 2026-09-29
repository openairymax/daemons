/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.5.0 (L3 SSoT) 生成。
 * 机制层装配；策略层在 src/svc.c 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#include "platform.h"
#include "airy_rt.h"
#include "svc_think_d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daemon_main.h"
#include "daemon_ipc_ops_bootstrap.h"
#include "daemon_llm_ops_bootstrap.h"
#include "daemon_tool_ops_bootstrap.h"

DAEMON_DECLARE_COMMON(think_d, think,
                      THINK_D_SOCKET_UNIX, THINK_D_SOCKET_WIN,
                      THINK_D_TCP_PORT, THINK_D_MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(think_d)

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp,
                                     print_usage_think_d);
    if (parse_rc > 0) return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_think_d);

#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler_think_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(think_d);
#endif

    airy_logger_config_t log_cfg = {0};
    const char *dbg = getenv("AIRY_THINK_D_DEBUG");
    log_cfg.level = (dbg && dbg[0] == '1') ? (log_level_t)LOG_LEVEL_DEBUG :
                     (log_level_t)LOG_LEVEL_WARN;
    airy_log_init(&log_cfg);
    atexit(log_cleanup);

    int core_ret = airy_init();
    if (core_ret == AIRY_SUCCESS)
        SVC_LOG_INFO("corekern core initialized (think_d runs on corekern)");
    else
        SVC_LOG_WARN("corekern init failed (%d), degraded (badge=0)", core_ret);

    daemon_cupolas_init_pep("think_d");
    daemon_ipc_ops_init("think_d");
    daemon_llm_ops_init("think_d");
    daemon_tool_ops_init("think_d");

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
        .max_events = 16, .thread_pool_min = 2,
        .thread_pool_max = 4, .thread_pool_queue_size = 32,
        .use_jsonrpc = true,
        .concurrent_clients = true,
        .on_client = daemon_on_client_think_d,
    };

    const char *sock_addr = ep.use_tcp ? ep.tcp_host : ep.sock_unix;
    int ret = daemon_init_event_driver(
        "think_d", "think", sock_addr,
        ep.use_tcp ? ep.tcp_port : 0, "think,core",
        ep.use_tcp, &ev_config, &g_event_driver_think_d, &g_bsd_think_d,
        &g_bipc_think_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_think_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        goto fail_svc;
    }

    g_dispatcher_think_d = daemon_event_driver_get_dispatcher(
        g_event_driver_think_d);
    static const daemon_method_entry_t SVC_METHODS[] = {
        {"process", m_process},
        {"orchestrate", m_orchestrate},
        {"get_stats", m_get_stats},
        {"health_check", m_health_check},
        {"lang_process", m_lang_process},
        {"lang_postprocess", m_lang_postprocess},
        {"lang_stats", m_lang_stats},
        {"review", m_review},
        {"shutdown", on_shutdown_method_think_d},
    };
    DAEMON_REGISTER_METHODS(g_dispatcher_think_d, SVC_METHODS);
    SVC_LOG_INFO("Registered 9 RPC methods (think.* namespace)");
    svc_attach(g_dispatcher_think_d);

    if (daemon_event_driver_add_server_fd(g_event_driver_think_d,
                                          (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        goto fail_driver;
    }

    if (svc_activate(g_event_driver_think_d, g_bsd_think_d) != 0) {
        SVC_LOG_ERROR("Service activate failed");
        goto fail_driver;
    }

    SVC_LOG_INFO("think service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_think_d);

    svc_teardown();
    daemon_cleanup_standard(g_bipc_think_d, g_bsd_think_d,
                            g_event_driver_think_d, server_fd,
                            ep.sock_unix, svc_destroy,
                            &g_running_lock_think_d);
    daemon_tool_ops_cleanup();
    daemon_llm_ops_cleanup();
    daemon_ipc_ops_cleanup();
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;

fail_driver:
    daemon_event_driver_destroy(g_event_driver_think_d);
    airy_sock_close(server_fd);
fail_svc:
    svc_destroy();
    airy_mtx_destroy(&g_running_lock_think_d);
    airy_sock_cleanup();
    return EXIT_FAILURE;
}
