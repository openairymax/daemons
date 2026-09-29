/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.3.0 (L3 SSoT) 生成。
 * 机制层装配；策略层在 src/svc.c 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#include "platform.h"
#include "airy_rt.h"
#include "svc_cupolas_d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daemon_main.h"
#include "daemon_ipc_ops_bootstrap.h"

DAEMON_DECLARE_COMMON(cupolas_d, cupolas,
                      CUPOLAS_D_SOCKET_UNIX, CUPOLAS_D_SOCKET_WIN,
                      CUPOLAS_D_TCP_PORT, CUPOLAS_D_MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(cupolas_d)

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp,
                                     print_usage_cupolas_d);
    if (parse_rc > 0) return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_cupolas_d);

#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler_cupolas_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(cupolas_d);
#endif

    airy_logger_config_t log_cfg = {0};
    const char *dbg = getenv("AIRY_CUPOLAS_D_DEBUG");
    log_cfg.level = (dbg && dbg[0] == '1') ? (log_level_t)LOG_LEVEL_DEBUG :
                     (log_level_t)LOG_LEVEL_WARN;
    airy_log_init(&log_cfg);
    atexit(log_cleanup);

    int core_ret = airy_init();
    if (core_ret == AIRY_SUCCESS)
        SVC_LOG_INFO("corekern core initialized (cupolas_d runs on corekern)");
    else
        SVC_LOG_WARN("corekern init failed (%d), degraded (badge=0)", core_ret);

    daemon_cupolas_init("cupolas_d");
    daemon_ipc_ops_init("cupolas_d");

    if (svc_prepare_cupolas_d(config_path) != 0) {
        SVC_LOG_ERROR("Service prepare failed");
        goto fail_svc;
    }

    daemon_endpoint_t ep;
    svc_endpoint_cupolas_d(&ep, use_tcp);

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
        .on_client = daemon_on_client_cupolas_d,
    };

    const char *sock_addr = ep.use_tcp ? ep.tcp_host : ep.sock_unix;
    int ret = daemon_init_event_driver(
        "cupolas_d", "cupolas", sock_addr,
        ep.use_tcp ? ep.tcp_port : 0, "cupolas,security",
        ep.use_tcp, &ev_config, &g_event_driver_cupolas_d, &g_bsd_cupolas_d,
        &g_bipc_cupolas_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_cupolas_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        goto fail_svc;
    }

    g_dispatcher_cupolas_d = daemon_event_driver_get_dispatcher(
        g_event_driver_cupolas_d);
    static const daemon_method_entry_t SVC_METHODS[] = {
        {"check_permission", svc_on_check_permission_cupolas_d},
        {"sanitize", svc_on_sanitize_cupolas_d},
        {"execute_command", svc_on_execute_command_cupolas_d},
        {"add_rule", svc_on_add_rule_cupolas_d},
        {"audit_flush", svc_on_audit_flush_cupolas_d},
        {"health_check", svc_on_health_check_cupolas_d},
        {"get_stats", svc_on_get_stats_cupolas_d},
        {"vault_store", svc_on_vault_store_cupolas_d},
        {"vault_retrieve", svc_on_vault_retrieve_cupolas_d},
        {"vault_delete", svc_on_vault_delete_cupolas_d},
        {"vault_list", svc_on_vault_list_cupolas_d},
        {"vault_rotate", svc_on_vault_rotate_cupolas_d},
        {"net_add_rule", svc_on_net_add_rule_cupolas_d},
        {"net_check_access", svc_on_net_check_access_cupolas_d},
        {"net_get_stats", svc_on_net_get_stats_cupolas_d},
        {"entitlements_load", svc_on_entitlements_load_cupolas_d},
        {"entitlements_check", svc_on_entitlements_check_cupolas_d},
        {"policy_load", svc_on_policy_load_cupolas_d},
        {"policy_activate", svc_on_policy_activate_cupolas_d},
        {"policy_rollback", svc_on_policy_rollback_cupolas_d},
        {"policy_status", svc_on_policy_status_cupolas_d},
        {"shutdown", on_shutdown_method_cupolas_d},
    };
    DAEMON_REGISTER_METHODS(g_dispatcher_cupolas_d, SVC_METHODS);
    SVC_LOG_INFO("Registered 22 RPC methods (cupolas.* namespace)");
    svc_attach_cupolas_d(g_dispatcher_cupolas_d);

    if (daemon_event_driver_add_server_fd(g_event_driver_cupolas_d,
                                          (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        goto fail_driver;
    }

    if (svc_activate_cupolas_d(g_event_driver_cupolas_d) != 0) {
        SVC_LOG_ERROR("Service activate failed");
        goto fail_driver;
    }

    SVC_LOG_INFO("cupolas service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_cupolas_d);

    svc_teardown_cupolas_d();
    daemon_cleanup_standard(g_bipc_cupolas_d, g_bsd_cupolas_d,
                            g_event_driver_cupolas_d, server_fd,
                            CUPOLAS_D_SOCKET_UNIX, svc_destroy_cupolas_d,
                            &g_running_lock_cupolas_d);
    daemon_ipc_ops_cleanup();
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;

fail_driver:
    daemon_event_driver_destroy(g_event_driver_cupolas_d);
    airy_sock_close(server_fd);
fail_svc:
    svc_destroy_cupolas_d();
    airy_mtx_destroy(&g_running_lock_cupolas_d);
    airy_sock_cleanup();
    return EXIT_FAILURE;
}
