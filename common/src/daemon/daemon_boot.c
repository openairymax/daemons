/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_boot.c
 * @brief Daemon 启动机制：parse -> init -> serve -> cleanup 全装配序。
 *
 * 0.1.19 §79：十二个生成户 main.c 的横向装配副本（daemon_gen.py v1.8.0
 * 模板克隆源，daemons/dup 主要成分）收敛为本机制的单一实现。各户 main.c
 * 只保留策略面（daemon_boot_t 策略包填充），装配细节全部随本文件私有：
 *   - 参数解析（--manager/--tcp/--help，Windows 强制 TCP 回环）
 *   - 信号注册（POSIX 全集 / Windows SetConsoleCtrlHandler）
 *   - 调试日志开关（AIRY_<D>_DEBUG -> LOG_LEVEL_DEBUG）
 *   - 套接字创建（TCP/Unix/Win 命名管道分派）
 *   - 事件驱动 + SD bootstrap 引导
 *   - 方法表注册与 ops 正序 init / 逆序 cleanup
 *   - 标准清理链与 fail_driver/fail_svc 错误路径
 *
 * 装配序与错误路径逐行对齐 v1.8.0 模板（消解不改行为）。
 *
 * @see daemon_main.h daemon_boot_t（策略包字段契约）
 */

#include "airy_rt.h"
#include "daemon_main.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int daemon_parse_args(int argc, char **argv, const char **config_path, int *use_tcp,
                             void (*print_usage_fn)(const char *))
{
#if defined(AIRY_PLATFORM_WINDOWS)
    /* Windows：IPC 统一走 TCP 回环（事件循环仅支持 socket，命名管道
     * 无法接入 WSAEventSelect）；--tcp 参数保留为显式语义一致性。 */
    *use_tcp = 1;
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--manager") == 0 && i + 1 < argc) {
            *config_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            if (print_usage_fn)
                print_usage_fn(argv[0]);
            return 1; /* exit(0) */
        } else if (strcmp(argv[i], "--tcp") == 0) {
            *use_tcp = 1;
        } else {
            SVC_LOG_ERROR("Unknown option: %s", argv[i]);
            if (print_usage_fn)
                print_usage_fn(argv[0]);
            return 2; /* exit(1) */
        }
    }
    return 0;
}

static airy_sock_t daemon_listen_sock(int use_tcp, int tcp_port, const char *unix_path,
                                      const char *win_pipe)
{
    if (use_tcp) {
        return airy_sock_create_tcp_server("127.0.0.1", tcp_port);
    }
#if defined(AIRY_PLATFORM_WINDOWS)
    /* Windows：事件循环（WSAEventSelect）仅接受 socket，命名管道句柄
     * 无法接入；daemon 统一走 TCP 回环（parse_args 亦强制 use_tcp）。 */
    (void)unix_path;
    (void)win_pipe;
    return airy_sock_create_tcp_server("127.0.0.1", tcp_port);
#else
    (void)win_pipe;
    return airy_sock_create_unix_server(unix_path);
#endif
}

/* svc 钩子缺省策略（null object，0.1.19 §80）：无对应策略需求的户经
 * DAEMON_BOOT_WIRE 引用本组符号，svc.c 不再逐户维护空桩副本。行为与
 * 原空桩逐字等价：立即成功返回，不触碰入参。 */
int daemon_svc_activate_noop(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd)
{
    (void)driver;
    (void)bsd;
    return 0;
}

void daemon_svc_attach_noop(void *dispatcher)
{
    (void)dispatcher;
}

void daemon_svc_teardown_noop(void)
{
}

static int daemon_init_driver(const char *daemon_name, const char *service_type,
                              const char *socket_path, int tcp_port, const char *tags,
                              int use_tcp, const daemon_event_config_t *ev_config,
                              daemon_event_driver_t **p_event_driver,
                              daemon_bootstrap_sd_t **p_bsd)
{
    if (p_bsd) {
        const char *sd_addr = use_tcp ? "127.0.0.1" : socket_path;
        *p_bsd = daemon_bootstrap_sd_start(daemon_name, service_type, sd_addr, tcp_port, tags, 0);
    }

    if (!ev_config || !p_event_driver)
        return AIRY_ERR_INVALID_PARAM;
    *p_event_driver = daemon_event_driver_create(ev_config);
    if (!*p_event_driver)
        return AIRY_ERR_OUT_OF_MEMORY;

    return AIRY_SUCCESS;
}

static void daemon_cleanup(daemon_bootstrap_sd_t *bsd,
                           daemon_event_driver_t *event_driver, airy_sock_t server_fd,
                           const char *unix_socket_path, void (*destroy_service)(void),
                           airy_mtx_t *running_lock)
{
    SVC_LOG_WARN("Service stopping...");

    if (bsd)
        daemon_bootstrap_sd_stop(bsd);
    if (event_driver)
        daemon_event_driver_destroy(event_driver);
    if (server_fd >= 0)
        airy_sock_close(server_fd);
#if AIRY_PLATFORM_POSIX
    /* Unlink the listening socket file after closing the fd. Ignore
     * ENOENT: TCP mode or an already-removed file is normal. */
    if (unix_socket_path && unix_socket_path[0])
        (void)unlink(unix_socket_path);
#endif
    if (destroy_service)
        destroy_service();
    if (running_lock)
        airy_mtx_destroy(running_lock);
    airy_sock_cleanup();

    SVC_LOG_WARN("Service stopped");
}

int daemon_boot(int argc, char **argv, const daemon_boot_t *boot)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc =
        daemon_parse_args(argc, argv, &config_path, &use_tcp, boot->print_usage);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(boot->running_lock);

#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)boot->signal_handler, TRUE);
#else
    signal(SIGINT, boot->signal_handler);
    signal(SIGTERM, boot->signal_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, boot->log_toggle);
#endif

    log_config_t log_cfg = {0};
    const char *dbg = getenv(boot->env_debug);
    log_cfg.level =
        (dbg && dbg[0] == '1') ? LOG_LEVEL_DEBUG : LOG_LEVEL_WARN;
    log_cfg.outputs = (1u << LOG_OUTPUT_CONSOLE);
    log_init(&log_cfg);
    atexit(log_cleanup);

    int core_ret = airy_init();
    if (core_ret == AIRY_SUCCESS)
        SVC_LOG_INFO("corekern core initialized (%s runs on corekern)", boot->daemon);
    else
        SVC_LOG_WARN("corekern init failed (%d), degraded (badge=0)", core_ret);

    if (boot->sec_init)
        boot->sec_init(boot->daemon);
    for (size_t i = 0; i < boot->ops_count; i++)
        boot->ops[i].init(boot->daemon);

    if (boot->svc_prepare(config_path) != 0) {
        SVC_LOG_ERROR("Service prepare failed");
        goto fail_svc;
    }

    daemon_endpoint_t ep;
    boot->svc_endpoint(&ep, use_tcp);

    airy_sock_t server_fd =
        daemon_listen_sock(ep.use_tcp, ep.tcp_port, ep.sock_unix, ep.sock_win);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        goto fail_svc;
    }

    daemon_event_config_t ev_config = {
        .max_events = boot->pool_max_events,
        .thread_pool_min = boot->pool_min,
        .thread_pool_max = boot->pool_max,
        .thread_pool_queue_size = boot->pool_queue,
        .concurrent_clients = boot->concurrent_clients,
        .use_jsonrpc = true,
        .on_client = boot->on_client,
    };

    const char *sock_addr = ep.use_tcp ? ep.tcp_host : ep.sock_unix;
    int ret = daemon_init_driver(boot->daemon, boot->sd_type, sock_addr,
                                       ep.use_tcp ? ep.tcp_port : 0, boot->tags, ep.use_tcp,
                                       &ev_config, boot->event_driver, boot->bsd);
    if (ret != AIRY_SUCCESS || !*boot->event_driver) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        goto fail_svc;
    }

    *boot->dispatcher = daemon_event_driver_get_dispatcher(*boot->event_driver);
    for (size_t i = 0; i < boot->method_count; i++)
        method_dispatcher_register(*boot->dispatcher, boot->methods[i].name,
                                   boot->methods[i].handler, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (%s.* namespace)", boot->method_total, boot->cname);
    boot->svc_attach(*boot->dispatcher);

    if (daemon_event_driver_add_server_fd(*boot->event_driver, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        goto fail_driver;
    }

    if (boot->svc_activate(*boot->event_driver, *boot->bsd) != 0) {
        SVC_LOG_ERROR("Service activate failed");
        goto fail_driver;
    }

    SVC_LOG_INFO("%s service running (event-driven mode)", boot->cname);
    daemon_event_driver_run(*boot->event_driver);

    boot->svc_teardown();
    daemon_cleanup(*boot->bsd, *boot->event_driver, server_fd, ep.sock_unix,
                            boot->svc_destroy, boot->running_lock);
    for (size_t i = boot->ops_count; i > 0; i--)
        boot->ops[i - 1].cleanup();
    if (boot->sec_cleanup)
        boot->sec_cleanup();
    log_cleanup();
    return 0;

fail_driver:
    daemon_event_driver_destroy(*boot->event_driver);
    airy_sock_close(server_fd);
fail_svc:
    boot->svc_destroy();
    airy_mtx_destroy(boot->running_lock);
    airy_sock_cleanup();
    return EXIT_FAILURE;
}
