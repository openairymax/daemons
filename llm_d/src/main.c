// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file main.c
 * @brief LLM service daemon entry & wiring (daemon module conventions).
 *
 * 2026-08-27 域拆分（原 1033 行 → 4 文件）：本文件仅保留入口引导——
 * daemon_main.h 宏实例化（生成的 g_running_llm_d / signal_handler 等
 * static 样板必须与引用它们的接线代码同 TU）、信号安装、日志初始化、
 * 方法注册与事件驱动循环；请求解析见 llm_daemon_request.c，RPC 方法
 * 见 llm_daemon_methods.c，daemon 配置装配见 llm_daemon_config.c，
 * 共享符号经 llm_service_internal.h 声明。
 *
 * Conventions followed:
 * - ARCHITECTURAL_PRINCIPLES.md E-3 resource determinism (paired management)
 * - ARCHITECTURAL_PRINCIPLES.md E-4 cross-platform consistency (platform.h)
 * - ARCHITECTURAL_PRINCIPLES.md E-5 semantic naming (SVC_LOG_*)
 * - ARCHITECTURAL_PRINCIPLES.md E-6 traceable errors (AIRY_ERR_*)
 */

/* P0.18.1: daemon_main.h provides DAEMON_DECLARE_COMMON/DAEMON_SETUP_SIGNALS/
 * daemon_parse_args/daemon_create_server_socket/daemon_init_event_driver/
 * daemon_cleanup_standard boilerplate macros and inline helpers. It
 * transitively includes atomic_compat.h, daemon_bootstrap_*.h,
 * daemon_cupolas_bootstrap.h, daemon_event_driver.h, daemon_platform_ext.h,
 * jsonrpc_helpers.h, logging.h, method_dispatcher.h, svc_logger.h,
 * cjson/cJSON.h, cjson_helpers.h, so only business-logic headers are kept
 * here. */
#include "platform.h"
#include "llm_service_internal.h"

#include <stdio.h>
#include <stdlib.h>

#include "daemon_main.h"

/* P0.18.1: generate the common globals (g_running_llm_d etc.), signal
 * handling (signal_handler_llm_d, svc_log_toggle_handler_llm_d),
 * print_usage_llm_d, daemon_handle_client_llm_d, daemon_on_client_llm_d
 * boilerplate, eliminating hand-written duplication. */
DAEMON_DECLARE_COMMON(llm_d, llm, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(llm_d)

llm_service_t *g_service = NULL;

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_llm_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    /* Model SSoT fallback: when no --manager is given (e.g. started by the
     * CLI /daemon command or a plain exec), load $AIRY_CONFIG_DIR/model.yaml
     * so the provider registry / llm_router always see the configured
     * endpoints. Same pattern as think_d / gateway_d reading model.yaml from
     * airy_config_dir(). Kept in a static buffer for the process lifetime. */
    if (!config_path) {
        static char default_model_path[1024];
        const char *cfg_dir = airy_config_dir();
        if (cfg_dir) {
            int plen = snprintf(default_model_path, sizeof(default_model_path), "%s/model.yaml",
                                cfg_dir);
            if (plen > 0 && plen < (int)sizeof(default_model_path))
                config_path = default_model_path;
        }
    }

    airy_sock_init();
    airy_mtx_init(&g_running_lock_llm_d);

#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler_llm_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(llm_d);
#endif

    /* Keep the initial log level WARN (SIGUSR1 toggles between DEBUG/INFO,
     * see the generated svc_log_toggle_handler_llm_d). Debugging:
     * AIRY_LLM_D_DEBUG=1 outputs DEBUG-level logs */
    airy_logger_config_t log_cfg = {0};
    const char *dbg = getenv("AIRY_LLM_D_DEBUG");
    log_cfg.level = (dbg && dbg[0] == '1') ? (log_level_t)LOG_LEVEL_DEBUG :
                                             (log_level_t)LOG_LEVEL_WARN;
    airy_log_init(&log_cfg);
    atexit(log_cleanup);

    daemon_cupolas_init_pep("llm_d");

    load_daemon_config(config_path);
    use_tcp = use_tcp || g_config.use_tcp;

    SVC_LOG_INFO("LLM service starting, manager=%s", config_path ? config_path : "default");

    g_service = llm_service_create(config_path);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create service");
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    int tcp_port = g_config.tcp_port ? (int)g_config.tcp_port : DEFAULT_TCP_PORT;
    const char *unix_path = g_config.socket_path ? g_config.socket_path : DEFAULT_SOCKET_PATH_UNIX;
    airy_sock_t server_fd =
        daemon_create_server_socket(use_tcp, tcp_port, unix_path, DEFAULT_SOCKET_PATH_WIN);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service_llm_d();
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    if (use_tcp)
        SVC_LOG_INFO("Listening on TCP port %d", tcp_port);
    else
        SVC_LOG_INFO("Listening on %s", unix_path);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = g_config.max_threads > 0 ? g_config.max_threads : 4;
    ev_config.thread_pool_max = g_config.max_threads > 0 ? g_config.max_threads : 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    /* 0.1.6h：llm_d complete_stream 是长流（每 chunk 持续写 socket）。
     * 原同步模式在事件循环线程内阻塞流式写：长思考期间其他连接排队、
     * CLI health_check（6s 超时）误报掉线、健康定时器失效——表现为
     * "daemon 总是掉线"。开启并发，把 on_client 提交线程池处理。 */
    ev_config.concurrent_clients = true;
    ev_config.on_client = daemon_on_client_llm_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = use_tcp ? "127.0.0.1" : unix_path;
    int ret = daemon_init_event_driver("llm_d", "llm", sock_addr, use_tcp ? tcp_port : 0, "ai,core",
                                       use_tcp, &ev_config, &g_event_driver_llm_d, &g_bsd_llm_d,
                                       &g_bipc_llm_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_llm_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service_llm_d();
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_llm_d = daemon_event_driver_get_dispatcher(g_event_driver_llm_d);
    method_dispatcher_register(g_dispatcher_llm_d, "complete", on_complete_method, NULL);
    method_dispatcher_register(g_dispatcher_llm_d, "complete_stream", on_complete_stream_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_llm_d, "list_models", on_list_models_method, NULL);
    /* Standard L2 protocol methods (02-l2-service-protocol.md:
     * llm.count_tokens / llm.health_check / llm.get_stats)
     */
    method_dispatcher_register(g_dispatcher_llm_d, "count_tokens", on_count_tokens_method, NULL);
    method_dispatcher_register(g_dispatcher_llm_d, "health_check", on_health_check_method, NULL);
    method_dispatcher_register(g_dispatcher_llm_d, "get_stats", on_get_stats_method, NULL);
    method_dispatcher_register(g_dispatcher_llm_d, "embeddings", on_embeddings_method, NULL);

    method_dispatcher_register(g_dispatcher_llm_d, "shutdown", on_shutdown_method_llm_d, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (llm.* namespace)", 8);

    if (daemon_event_driver_add_server_fd(g_event_driver_llm_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_llm_d);
        airy_sock_close(server_fd);
        destroy_service_llm_d();
        airy_mtx_destroy(&g_running_lock_llm_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("LLM service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_llm_d);

    daemon_cleanup_standard(g_bipc_llm_d, g_bsd_llm_d, g_event_driver_llm_d, server_fd, unix_path,
                            destroy_service_llm_d, &g_running_lock_llm_d);

    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
