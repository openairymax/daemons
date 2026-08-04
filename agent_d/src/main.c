#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file main.c
 * @brief Agent 服务守护进程主入口（遵循 daemon 模块统一规范）
 *
 * 暴露 JSON-RPC 方法（agent.* 命名空间）：
 *   - agent.spawn     : 派生新 Agent
 *   - agent.terminate : 终止指定 Agent
 *   - agent.invoke    : 调用指定 Agent
 *   - agent.list      : 列出所有 Agent ID
 *   - agent.count     : 返回当前 Agent 数（健康检查辅助）
 *
 * Unix socket 路径：${AIRY_RUNTIME_DIR}/agent.sock
 */

#include "daemon_main.h"
#include "agent_service.h"
#include "param_validator.h"
#include "svc_logger.h"
#include "thread_pool.h"
#include "platform.h"

#include <stdlib.h>

/* ==================== 配置常量 ==================== */

#define DEFAULT_SOCKET_PATH_UNIX AIRY_RUNTIME_DIR "/agent.sock"
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_agent"
#define DEFAULT_TCP_PORT 8086
#define MAX_BUFFER 65536
#define MAX_CLIENTS 2048
#define AGENT_DEFAULT_MAX_AGENTS 10000

/* 生成公共全局变量、信号处理、help、客户端处理等样板 */
DAEMON_DECLARE_COMMON(agent_d, agent, DEFAULT_SOCKET_PATH_UNIX,
                       DEFAULT_SOCKET_PATH_WIN, DEFAULT_TCP_PORT, MAX_BUFFER)

/* ==================== 全局状态 ==================== */

static agent_service_t *g_service = NULL;

/* daemon 配置（max_agents 等），供启动/监控线程使用 */
typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_clients;
    size_t max_agents;
} agent_daemon_config_t;

static agent_daemon_config_t g_config = {0};

#if AIRY_PLATFORM_POSIX
/* P0-3：空闲 Agent 子进程回收。
 * 守护线程周期性调用 agent_service_reap_idle，终止超过空闲阈值
 * （AIRY_AGENT_IDLE_TIMEOUT_S，默认 300s）仍无调用的子进程，
 * 防止 Python runner 进程泄漏（历史上达 12 个空闲进程残留）。 */
static volatile int g_reaper_run = 0;
static airy_thread_t g_reaper_thread = AIRY_INVALID_THREAD;

static void *idle_reaper_thread(void *arg)
{
    (void)arg;
    const char *env_timeout = getenv("AIRY_AGENT_IDLE_TIMEOUT_S");
    uint64_t max_idle_s = 300;
    if (env_timeout && env_timeout[0] != '\0') {
        unsigned long long v = strtoull(env_timeout, NULL, 10);
        if (v > 0)
            max_idle_s = (uint64_t)v;
    }
    SVC_LOG_INFO("Idle reaper started (max_idle=%llus, scan every %ds)",
                 (unsigned long long)max_idle_s, 30);
    int slept = 0;
    while (g_reaper_run) {
        /* 1s 步进休眠，便于收到退出信号后快速 join */
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        if (!g_reaper_run)
            break;
        if (++slept >= 30) {
            slept = 0;
            if (g_service)
                agent_service_reap_idle(g_service, max_idle_s);
        }
    }
    return NULL;
}

static void idle_reaper_start(void)
{
    g_reaper_run = 1;
    if (airy_thread_create(&g_reaper_thread, idle_reaper_thread, NULL) != 0) {
        g_reaper_run = 0;
        SVC_LOG_WARN("Failed to start idle reaper thread");
    }
}

static void idle_reaper_stop(void)
{
    g_reaper_run = 0;
    if (g_reaper_thread != AIRY_INVALID_THREAD) {
        airy_thread_join(g_reaper_thread, NULL);
        g_reaper_thread = AIRY_INVALID_THREAD;
    }
}

/* ==================== 性能监控 ==================== */

/* 单调时钟微秒（POSIX: clock_gettime；Windows: GetTickCount64 换算） */
static uint64_t perf_now_us(void)
{
#if AIRY_PLATFORM_POSIX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#else
    return (uint64_t)GetTickCount64() * 1000ull;
#endif
}

/* 慢请求判定阈值（微秒）：AIRY_AGENT_PERF_SLOW_US，默认 1s */
static int64_t perf_slow_threshold_us(void)
{
    const char *env = getenv("AIRY_AGENT_PERF_SLOW_US");
    if (env && env[0] != '\0') {
        long long v = strtoll(env, NULL, 10);
        if (v > 0)
            return v;
    }
    return 1000000;
}

/* 周期采样线程：聚合 service 层原子计数器 + 线程池状态，输出一行
 * [PERF] 摘要。10000 并发下逐请求日志会刷爆 IO，因此只做窗口聚合：
 *   - spawn/invoke/terminate 窗口增量与累计成败
 *   - spawn/invoke 平均/最大时延（微秒）
 *   - 全局锁竞争次数（lock_wait，trylock 探测）
 *   - 当前 agent 数 / 峰值并发 / 线程池 active 与 pending（队列深度）
 * 采样间隔：AIRY_AGENT_PERF_INTERVAL_S，默认 5s。 */
static volatile int g_perf_run = 0;
static airy_thread_t g_perf_thread = AIRY_INVALID_THREAD;

static void *perf_monitor_thread(void *arg)
{
    (void)arg;
    const char *env_interval = getenv("AIRY_AGENT_PERF_INTERVAL_S");
    int interval_s = 5;
    if (env_interval && env_interval[0] != '\0') {
        long v = strtol(env_interval, NULL, 10);
        if (v > 0 && v <= 3600)
            interval_s = (int)v;
    }
    SVC_LOG_INFO("Perf monitor started (interval=%ds, slow_threshold_us=%lld)",
                 interval_s, (long long)perf_slow_threshold_us());

    agent_perf_stats_t prev;
    __builtin_memset(&prev, 0, sizeof(prev));
    int slept = 0;
    while (g_perf_run) {
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        if (!g_perf_run)
            break;
        if (++slept < interval_s)
            continue;
        slept = 0;

        if (!g_service)
            continue;
        agent_perf_stats_t cur;
        if (agent_service_get_perf(g_service, &cur) != AIRY_SUCCESS)
            continue;

        int d_spawn = cur.spawn_total - prev.spawn_total;
        int d_invoke = cur.invoke_total - prev.invoke_total;
        int d_terminate = cur.terminate_total - prev.terminate_total;
        int d_lock = cur.lock_wait_total - prev.lock_wait_total;
        prev = cur;

        /* 线程池状态：active 线程数 / pending（待执行队列深度） */
        uint32_t pool_active = 0, pool_pending = 0;
        thread_pool_t *pool = g_event_driver_agent_d
                                  ? daemon_event_driver_get_pool(g_event_driver_agent_d)
                                  : NULL;
        if (pool) {
            pool_active = thread_pool_active_count(pool);
            pool_pending = thread_pool_pending_count(pool);
        }

        uint64_t spawn_avg = (cur.spawn_ok > 0)
            ? (uint64_t)(cur.spawn_us_total / (unsigned long long)cur.spawn_ok)
            : 0;
        uint64_t invoke_avg = (cur.invoke_ok > 0)
            ? (uint64_t)(cur.invoke_us_total / (unsigned long long)cur.invoke_ok)
            : 0;

        SVC_LOG_INFO(
            "[PERF] window=%ds spawn{+%d total=%d ok=%d fail=%d avg_us=%llu max_us=%llu} "
            "invoke{+%d total=%d ok=%d fail=%d avg_us=%llu max_us=%llu} "
            "terminate{+%d total=%d} lock_wait{+%d total=%d} "
            "agents=%zu/%zu peak_running=%d pool{active=%u pending=%u}",
            interval_s,
            d_spawn, cur.spawn_total, cur.spawn_ok, cur.spawn_fail,
            (unsigned long long)spawn_avg, cur.spawn_us_max,
            d_invoke, cur.invoke_total, cur.invoke_ok, cur.invoke_fail,
            (unsigned long long)invoke_avg, cur.invoke_us_max,
            d_terminate, cur.terminate_total,
            d_lock, cur.lock_wait_total,
            agent_service_count(g_service), g_config.max_agents,
            cur.peak_running, pool_active, pool_pending);
    }
    return NULL;
}

static void perf_monitor_start(void)
{
    g_perf_run = 1;
    if (airy_thread_create(&g_perf_thread, perf_monitor_thread, NULL) != 0) {
        g_perf_run = 0;
        SVC_LOG_WARN("Failed to start perf monitor thread");
    }
}

static void perf_monitor_stop(void)
{
    g_perf_run = 0;
    if (g_perf_thread != AIRY_INVALID_THREAD) {
        airy_thread_join(g_perf_thread, NULL);
        g_perf_thread = AIRY_INVALID_THREAD;
    }
}
#endif /* AIRY_PLATFORM_POSIX */

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

/* ==================== 请求处理方法 ==================== */

static void handle_spawn(cJSON *params, int id, airy_sock_t fd);
static void handle_terminate(cJSON *params, int id, airy_sock_t fd);
static void handle_invoke(cJSON *params, int id, airy_sock_t fd);
static void handle_list(int id, airy_sock_t fd);
static void handle_count(int id, airy_sock_t fd);

static void on_spawn_method(cJSON *params, int id, void *user_data)
{
    handle_spawn(params, id, *(airy_sock_t *)user_data);
}

static void on_terminate_method(cJSON *params, int id, void *user_data)
{
    handle_terminate(params, id, *(airy_sock_t *)user_data);
}

static void on_invoke_method(cJSON *params, int id, void *user_data)
{
    handle_invoke(params, id, *(airy_sock_t *)user_data);
}

static void on_list_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_list(id, *(airy_sock_t *)user_data);
}

static void on_count_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_count(id, *(airy_sock_t *)user_data);
}

static void handle_spawn(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *spec = cJSON_GetObjectItem(params, "agent_spec");

    if (!spec) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_spec", id);
        return;
    }

    char *spec_str = NULL;
    if (cJSON_IsString(spec)) {
        spec_str = AIRY_STRDUP(spec->valuestring);
    } else {
        spec_str = cJSON_PrintUnformatted(spec);
    }
    if (!spec_str) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Invalid agent_spec", id);
        return;
    }

    uint64_t perf_t0 = perf_now_us();
    char *out_agent_id = NULL;
    int ret = agent_service_spawn(g_service, spec_str, &out_agent_id);
    AIRY_FREE(spec_str);

    /* 慢请求监控：超过阈值（默认 1s）时打 WARN，便于定位冷启动/资源瓶颈 */
    {
        uint64_t elapsed = perf_now_us() - perf_t0;
        int64_t slow_us = perf_slow_threshold_us();
        if ((int64_t)elapsed > slow_us)
            SVC_LOG_WARN("agent.spawn slow: %llu us (threshold=%lld us)",
                         (unsigned long long)elapsed, (long long)slow_us);
    }

    if (ret != AIRY_SUCCESS || !out_agent_id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Agent spawn failed", id);
        SVC_LOG_ERROR("agent.spawn failed: error=%d", ret);
        AIRY_FREE(out_agent_id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "agent_id", out_agent_id);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    AIRY_FREE(out_agent_id);
}

static void handle_terminate(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_id = cJSON_GetObjectItem(params, "agent_id");

    if (!agent_id || !cJSON_IsString(agent_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }

    int ret = agent_service_terminate(g_service, agent_id->valuestring);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Agent not found", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "terminated", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_invoke(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_id = cJSON_GetObjectItem(params, "agent_id");
    cJSON *input = cJSON_GetObjectItem(params, "input");

    if (!agent_id || !cJSON_IsString(agent_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }

    const char *input_str = input && cJSON_IsString(input) ? input->valuestring : "";
    size_t input_len = strlen(input_str);

    uint64_t perf_t0 = perf_now_us();
    char *out_output = NULL;
    int ret = agent_service_invoke(g_service, agent_id->valuestring,
                                     input_str, input_len, &out_output);

    /* 慢请求监控：invoke 含子进程 LLM 往返，超过阈值（默认 1s）打 WARN */
    {
        uint64_t elapsed = perf_now_us() - perf_t0;
        int64_t slow_us = perf_slow_threshold_us();
        if ((int64_t)elapsed > slow_us)
            SVC_LOG_WARN("agent.invoke slow: %llu us (threshold=%lld us, agent_id=%s)",
                         (unsigned long long)elapsed, (long long)slow_us,
                         agent_id->valuestring);
    }

    if (ret == AIRY_SUCCESS && out_output) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "output", out_output);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        AIRY_FREE(out_output);
        return;
    }

    /* 错误路径：释放服务返回的错误 JSON，发送 JSON-RPC 错误 */
    AIRY_FREE(out_output);
    if (ret == AIRY_ERR_NOT_FOUND) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Agent not found", id);
    } else {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Agent invoke failed", id);
    }
    SVC_LOG_ERROR("agent.invoke failed: error=%d", ret);
}

static void handle_list(int id, airy_sock_t client_fd)
{
    char **agent_ids = NULL;
    size_t count = 0;

    int ret = agent_service_list(g_service, &agent_ids, &count);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Agent list failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(agent_ids[i]));
    }
    cJSON_AddItemToObject(result, "agent_ids", arr);
    cJSON_AddNumberToObject(result, "total", (double)count);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    agent_service_list_free(agent_ids, count);
}

static void handle_count(int id, airy_sock_t client_fd)
{
    size_t n = agent_service_count(g_service);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "count", (double)n);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ==================== 配置加载 ==================== */

static int load_daemon_config(const char *config_path)
{
    g_config.use_tcp = 0;
    g_config.max_clients = MAX_CLIENTS;
    g_config.max_agents = AGENT_DEFAULT_MAX_AGENTS;

#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_WIN);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#else
    /* Unix socket 收敛到 AIRY_HOME/run（生产部署不依赖 /tmp/agentrt） */
    {
        char sock_buf[AIRY_PATH_MAX];
        snprintf(sock_buf, sizeof(sock_buf), "%s/agent.sock",
                 airy_runtime_dir());
        g_config.socket_path = AIRY_STRDUP(sock_buf);
    }
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#endif
    g_config.tcp_port = DEFAULT_TCP_PORT;

    /* 环境变量覆盖：AIRY_MAX_AGENTS */
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

/* ==================== 主函数 ==================== */

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_agent_d);
    if (parse_rc > 0) return parse_rc == 1 ? 0 : 1;

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

    /* P3.14 ACC-DT15: 初始化 cupolas 安全穹顶 */
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

    airy_sock_t server_fd = daemon_create_server_socket(
        g_config.use_tcp, g_config.tcp_port, g_config.socket_path, g_config.socket_path);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_agent_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO(g_config.use_tcp ? "Listening on TCP %s:%d" : "Listening on %s",
                 g_config.tcp_host, g_config.tcp_port);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 256;
    ev_config.thread_pool_min = 16;
    ev_config.thread_pool_max = 128;
    ev_config.thread_pool_queue_size = 4096;
    ev_config.use_jsonrpc = true;
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
    method_dispatcher_register(g_dispatcher_agent_d, "list", on_list_method, NULL);
    method_dispatcher_register(g_dispatcher_agent_d, "count", on_count_method, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (agent.* namespace)", 5);

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
    /* P0-3：启动空闲子进程回收守护线程 */
    idle_reaper_start();
    /* 性能监控：周期采样线程（10000 并发验证资源瓶颈） */
    perf_monitor_start();
#endif
    daemon_event_driver_run(g_event_driver_agent_d);
#if AIRY_PLATFORM_POSIX
    /* 主循环退出：先停止监控与回收线程再清理服务 */
    perf_monitor_stop();
    idle_reaper_stop();
#endif

    daemon_cleanup_standard(g_bipc_agent_d, g_bsd_agent_d, g_event_driver_agent_d,
                             server_fd, destroy_service, &g_running_lock_agent_d);
    free_daemon_config();

    SVC_LOG_INFO("Agent service stopped");
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
