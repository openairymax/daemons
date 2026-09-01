// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file main.c
 * @brief Agent service daemon main entry (daemon module conventions).
 *
 * Exposes JSON-RPC methods (agent.* namespace):
 *   - agent.spawn     : spawn a new agent
 *   - agent.terminate : terminate the given agent
 *   - agent.invoke    : invoke the given agent
 *   - agent.list      : list all agent IDs
 *   - agent.count     : current agent count (health-check helper)
 *
 * Unix socket path: ${AIRY_RUNTIME_DIR}/agent.sock
 *
 * 2026-08-27 域拆分（原 826 行 → 3 文件）：本文件仅保留入口引导——daemon
 * 宏样板、daemon 配置装配与 main() 事件驱动循环；RPC 方法见
 * agent_d_rpc.c，空闲回收/性能采样线程见 agent_d_monitor.c，共享符号经
 * agent_d_internal.h 声明。
 */

#include "daemon_main.h"
#include "agent_d_internal.h"
#include "platform.h"

#include <stdlib.h>
#include <time.h>

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("agent.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_agent"
#define DEFAULT_TCP_PORT 8086
#define MAX_BUFFER 65536
#define MAX_CLIENTS 2048
#define AGENT_DEFAULT_MAX_AGENTS 10000

DAEMON_DECLARE_COMMON(agent_d, agent, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(agent_d)

agent_service_t *g_service = NULL;
uint64_t g_start_time = 0;

agent_daemon_config_t g_config = {0};

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_agent_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static int load_daemon_config(const char *config_path)
{
    g_config.use_tcp = 0;
    g_config.max_clients = MAX_CLIENTS;
    g_config.max_agents = AGENT_DEFAULT_MAX_AGENTS;

#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_WIN);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#else

    {
        char sock_buf[AIRY_PATH_MAX];
        snprintf(sock_buf, sizeof(sock_buf), "%s/agent.sock", airy_runtime_dir());
        g_config.socket_path = AIRY_STRDUP(sock_buf);
    }
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#endif
    g_config.tcp_port = DEFAULT_TCP_PORT;

    const char *env = getenv("AIRY_MAX_AGENTS");
    if (env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 65536)
            g_config.max_agents = (size_t)v;
    }

    if (config_path) {
        FILE *f = fopen(config_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len > 0 && len < 1024 * 1024) {
                char *content = (char *)AIRY_MALLOC((size_t)len + 1);
                if (content) {
                    size_t read_len = fread(content, 1, (size_t)len, f);
                    if (read_len == (size_t)len) {
                        content[read_len] = '\0';
                        do {
                            CJSON_PARSE_GUARD(root, content, { break; });
                            cJSON *daemon_cfg = cJSON_GetObjectItem(root, "daemon");
                            if (daemon_cfg) {
                                cJSON *socket_path = cJSON_GetObjectItem(daemon_cfg, "socket_path");
                                if (cJSON_IsString(socket_path)) {
                                    AIRY_FREE(g_config.socket_path);
                                    g_config.socket_path = AIRY_STRDUP(socket_path->valuestring);
                                }
                                cJSON *tcp_port = cJSON_GetObjectItem(daemon_cfg, "tcp_port");
                                if (cJSON_IsNumber(tcp_port)) {
                                    g_config.tcp_port = (uint16_t)tcp_port->valueint;
                                    g_config.use_tcp = 1;
                                }
                                cJSON *max_clients = cJSON_GetObjectItem(daemon_cfg, "max_clients");
                                if (cJSON_IsNumber(max_clients))
                                    g_config.max_clients = max_clients->valueint;
                                cJSON *max_agents = cJSON_GetObjectItem(daemon_cfg, "max_agents");
                                if (cJSON_IsNumber(max_agents))
                                    g_config.max_agents = (size_t)max_agents->valuedouble;
                            }
                        } while (0);
                    }
                    AIRY_FREE(content);
                }
            }
            fclose(f);
        }
    }
    return 0;
}

static void free_daemon_config(void)
{
    AIRY_FREE(g_config.socket_path);
    AIRY_FREE(g_config.tcp_host);
    __builtin_memset(&g_config, 0, sizeof(g_config));
}

static void destroy_service(void)
{
    if (g_service) {
        agent_service_destroy(g_service);
        g_service = NULL;
    }
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    g_start_time = (uint64_t)time(NULL);

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_agent_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    airy_paths_init();
    airy_sock_init();
    airy_mtx_init(&g_running_lock_agent_d);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    DAEMON_SETUP_SIGNALS(agent_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    daemon_cupolas_init("agent_d");

    load_daemon_config(config_path);
    if (use_tcp)
        g_config.use_tcp = 1;

    SVC_LOG_INFO("Agent service starting, manager=%s", config_path ? config_path : "default");

    g_service = agent_service_create(g_config.max_agents);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create agent service");
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_agent_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    airy_sock_t server_fd = daemon_create_server_socket(g_config.use_tcp, g_config.tcp_port,
                                                        g_config.socket_path, g_config.socket_path);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_agent_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO(g_config.use_tcp ? "Listening on TCP %s:%d" : "Listening on %s", g_config.tcp_host,
                 g_config.tcp_port);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 256;
    ev_config.thread_pool_min = 16;
    ev_config.thread_pool_max = 128;
    ev_config.thread_pool_queue_size = 4096;
    ev_config.use_jsonrpc = true;
    /* Improvement 1 (cancellation drill-down): concurrent clients are
     * required. invoke is a long request (LLM round-trip up to 300s); with
     * synchronous per-request processing the event loop would block and the
     * agent.cancel request could never arrive — cross-process cancellation
     * depends on concurrent handling (same configuration as tool_d). */
    ev_config.concurrent_clients = true;
    ev_config.on_client = daemon_on_client_agent_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = g_config.use_tcp ? g_config.tcp_host : g_config.socket_path;
    int ret = daemon_init_event_driver("agent_d", "agent", sock_addr,
                                       g_config.use_tcp ? g_config.tcp_port : 0, "agent,core",
                                       g_config.use_tcp, &ev_config, &g_event_driver_agent_d,
                                       &g_bsd_agent_d, &g_bipc_agent_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_agent_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_agent_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_agent_d = daemon_event_driver_get_dispatcher(g_event_driver_agent_d);
    method_dispatcher_register(g_dispatcher_agent_d, "spawn", on_spawn_method, NULL);
    method_dispatcher_register(g_dispatcher_agent_d, "terminate", on_terminate_method, NULL);
    method_dispatcher_register(g_dispatcher_agent_d, "invoke", on_invoke_method, NULL);
    method_dispatcher_register(g_dispatcher_agent_d, "cancel", on_cancel_method, NULL);
    method_dispatcher_register(g_dispatcher_agent_d, "list", on_list_method, NULL);
    method_dispatcher_register(g_dispatcher_agent_d, "count", on_count_method, NULL);

    /* M1-1a 引擎下沉：agent.run（进程内引擎）/ agent.run_cancel（会话取消）
     * 由 agent_d 承载，gateway 仅转发（见 gateway_d 转发改造）。 */
    method_dispatcher_register(g_dispatcher_agent_d, "run", on_run_method, NULL);
    method_dispatcher_register(g_dispatcher_agent_d, "run_cancel", on_run_cancel_method, NULL);

    method_dispatcher_register(g_dispatcher_agent_d, "health_check", on_health_check_method, NULL);

    method_dispatcher_register(g_dispatcher_agent_d, "shutdown", on_shutdown_method_agent_d, NULL);

    method_dispatcher_register(g_dispatcher_agent_d, "get_stats", on_get_stats_method, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (agent.* namespace)", 11);

    if (daemon_event_driver_add_server_fd(g_event_driver_agent_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_agent_d);
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_agent_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("Agent service running (event-driven mode)");
#if AIRY_PLATFORM_POSIX

    idle_reaper_start();

    perf_monitor_start(g_event_driver_agent_d);
#endif
    daemon_event_driver_run(g_event_driver_agent_d);
#if AIRY_PLATFORM_POSIX

    perf_monitor_stop();
    idle_reaper_stop();
#endif

    daemon_cleanup_standard(g_bipc_agent_d, g_bsd_agent_d, g_event_driver_agent_d, server_fd,
                            g_config.socket_path, destroy_service, &g_running_lock_agent_d);
    free_daemon_config();

    SVC_LOG_INFO("Agent service stopped");
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
