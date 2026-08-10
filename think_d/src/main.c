// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#include "airy_memory.h"
#include "error.h"
/*
 * @file main.c
 * @brief 双思考系统守护进程主入口（think.* 命名空间）
 *
 * 暴露 JSON-RPC 方法：
 *   - think.process     : 双思考处理（prompt → plan + 思考事件 JSON）
 *   - think.get_stats   : 双思考统计（engine 健康 + 调用次数）
 *   - think.health_check: 服务健康检查
 *
 * Unix socket 路径：${AIRY_RUNTIME_DIR}/think.sock
 */

#include "daemon_main.h"
#include "platform.h"
#include "param_validator.h"
#include "svc_logger.h"
#include "thread_pool.h"
#include "think_service.h"
#include "svc_model_defaults.h"

#include <stdlib.h>
#include <strings.h>

/* ==================== 配置常量 ==================== */

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("think.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_think"
#define DEFAULT_TCP_PORT 8090
#define MAX_BUFFER 1048576
#define MAX_CLIENTS 16

/* 生成公共全局变量、信号处理、help、客户端处理等样板 */
DAEMON_DECLARE_COMMON(think_d, think, DEFAULT_SOCKET_PATH_UNIX,
                       DEFAULT_SOCKET_PATH_WIN, DEFAULT_TCP_PORT, MAX_BUFFER)

/* L2 标准方法 <ns>.shutdown：生成优雅退出处理器（02-l2-service-protocol.md §6.1） */
DAEMON_DECLARE_SHUTDOWN_METHOD(think_d)

/* ==================== 全局状态 ==================== */

static think_service_t *g_service = NULL;

typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_clients;
    uint32_t process_timeout_ms;
    int think_enabled;      /* 双思考开关（model.yaml think.enabled / env AIRY_THINK_ENABLED） */
    char think2_slow_model[128];   /* t2 慢思考（GRAD 模型 A） */
    char think1_fast_model[128];   /* t1-f 快思考（GRAD 模型 B） */
    char think1_prof_model[128];   /* t1-p 专业思考（GRAD 模型 C） */
} think_daemon_config_t;

static think_daemon_config_t g_config = {0};

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_think_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

/* ==================== 请求处理方法 ==================== */

static void handle_process(cJSON *params, int id, airy_sock_t fd);
static void handle_get_stats(cJSON *params, int id, airy_sock_t fd);
static void handle_health_check(cJSON *params, int id, airy_sock_t fd);

static void on_process_method(cJSON *params, int id, void *user_data)
{
    handle_process(params, id, *(airy_sock_t *)user_data);
}

static void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_get_stats(params, id, *(airy_sock_t *)user_data);
}

static void on_health_check_method(cJSON *params __attribute__((unused)), int id,
                                   void *user_data)
{
    handle_health_check(params, id, *(airy_sock_t *)user_data);
}

static void handle_process(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *prompt = cJSON_GetObjectItem(params, "prompt");
    if (!prompt || !cJSON_IsString(prompt) || !prompt->valuestring[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing prompt string", id);
        return;
    }

    think_process_result_t res = {0};
    int ret = think_service_process(g_service, prompt->valuestring, &res);
    if (ret != AIRY_SUCCESS || !res.json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Think process failed", id);
        think_result_free(&res);
        return;
    }

    /* result 为 JSON 字符串值（JSON-RPC result 可为任意 JSON 值） */
    cJSON *res_obj = cJSON_CreateString(res.json);
    think_result_free(&res);
    if (!res_obj) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, res_obj, id);
}

static void handle_get_stats(cJSON *params __attribute__((unused)), int id,
                             airy_sock_t client_fd)
{
    char *stats = think_service_stats_json(g_service);
    if (!stats) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Think stats unavailable", id);
        return;
    }
    cJSON *stats_obj = cJSON_Parse(stats);
    AIRY_FREE(stats);
    if (!stats_obj) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid stats JSON", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, stats_obj, id);
}

static void handle_health_check(cJSON *params __attribute__((unused)), int id,
                                airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(result, "service", "think_d");
    cJSON_AddBoolToObject(result, "healthy", think_service_ready(g_service) ? true : false);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ==================== 配置加载 ==================== */

static int load_daemon_config(const char *config_path)
{
    g_config.use_tcp = 0;
    g_config.max_clients = MAX_CLIENTS;
    g_config.process_timeout_ms = 120000;
    g_config.think_enabled = 1;

#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_WIN);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#else
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_UNIX);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#endif
    g_config.tcp_port = DEFAULT_TCP_PORT;

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
                            }
                            cJSON *think = cJSON_GetObjectItem(root, "think");
                            if (think) {
                                cJSON *s2 = cJSON_GetObjectItem(think, "think2_slow_model");
                                if (cJSON_IsString(s2) && s2->valuestring[0])
                                    AIRY_STRNCPY_TERM(g_config.think2_slow_model, s2->valuestring,
                                                      sizeof(g_config.think2_slow_model));
                                cJSON *verify = cJSON_GetObjectItem(think, "think1_fast_model");
                                if (cJSON_IsString(verify) && verify->valuestring[0])
                                    AIRY_STRNCPY_TERM(g_config.think1_fast_model,
                                                      verify->valuestring,
                                                      sizeof(g_config.think1_fast_model));
                                cJSON *expert = cJSON_GetObjectItem(think, "think1_prof_model");
                                if (cJSON_IsString(expert) && expert->valuestring[0])
                                    AIRY_STRNCPY_TERM(g_config.think1_prof_model,
                                                      expert->valuestring,
                                                      sizeof(g_config.think1_prof_model));
                                cJSON *timeout = cJSON_GetObjectItem(think, "timeout_ms");
                                if (cJSON_IsNumber(timeout))
                                    g_config.process_timeout_ms = (uint32_t)timeout->valueint;
                                cJSON *enabled = cJSON_GetObjectItem(think, "enabled");
                                if (cJSON_IsBool(enabled) || cJSON_IsNumber(enabled))
                                    g_config.think_enabled = cJSON_IsTrue(enabled) ? 1 : 0;
                            }
                        } while (0);
                    }
                    AIRY_FREE(content);
                }
            }
            fclose(f);
        }
    }

    /* ── 模型 SSoT：$AIRY_CONFIG_DIR/model.yaml 的 think 段（三角色唯一配置源）。
     * 优先级：env（AIRY_THINK_*）> model.yaml think 段 > -c JSON（兼容旧）> 默认。
     * 与 gateway_d 读 global 段同一模式（svc_model_defaults 公共层 libyaml）。 */
    {
        char model_path[1024];
        const char *cfg_dir = airy_config_dir();
        int have_model_yaml = 0;
        if (cfg_dir) {
            snprintf(model_path, sizeof(model_path), "%s/model.yaml", cfg_dir);
            have_model_yaml = 1;
        }
        svc_model_think_config_t think_cfg;
        __builtin_memset(&think_cfg, 0, sizeof(think_cfg));
        think_cfg.enabled = g_config.think_enabled; /* 保留 -c JSON 层结果（缺省 1） */
        if (have_model_yaml &&
            svc_model_defaults_think_from_yaml(model_path, &think_cfg) == 0) {
            g_config.think_enabled = think_cfg.enabled;
            if (think_cfg.think2_slow_model[0])
                AIRY_STRNCPY_TERM(g_config.think2_slow_model, think_cfg.think2_slow_model,
                                  sizeof(g_config.think2_slow_model));
            if (think_cfg.think1_fast_model[0])
                AIRY_STRNCPY_TERM(g_config.think1_fast_model, think_cfg.think1_fast_model,
                                  sizeof(g_config.think1_fast_model));
            if (think_cfg.think1_prof_model[0])
                AIRY_STRNCPY_TERM(g_config.think1_prof_model, think_cfg.think1_prof_model,
                                  sizeof(g_config.think1_prof_model));
            if (think_cfg.timeout_ms > 0)
                g_config.process_timeout_ms = think_cfg.timeout_ms;
        }

        /* env 临时覆盖（最高优先级） */
        const char *e;
        if ((e = getenv("AIRY_THINK_ENABLED")) && *e) {
            int b = (strcmp(e, "0") == 0 || strcasecmp(e, "false") == 0 ||
                     strcasecmp(e, "no") == 0)
                        ? 0
                        : 1;
            g_config.think_enabled = b;
        }
        if ((e = getenv("AIRY_THINK2_SLOW_MODEL")) && *e)
            AIRY_STRNCPY_TERM(g_config.think2_slow_model, e,
                              sizeof(g_config.think2_slow_model));
        if ((e = getenv("AIRY_THINK1_FAST_MODEL")) && *e)
            AIRY_STRNCPY_TERM(g_config.think1_fast_model, e,
                              sizeof(g_config.think1_fast_model));
        if ((e = getenv("AIRY_THINK1_PROF_MODEL")) && *e)
            AIRY_STRNCPY_TERM(g_config.think1_prof_model, e,
                              sizeof(g_config.think1_prof_model));
        if ((e = getenv("AIRY_THINK_TIMEOUT_MS")) && *e && atoi(e) > 0)
            g_config.process_timeout_ms = (uint32_t)atoi(e);
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
        think_service_destroy(g_service);
        g_service = NULL;
    }
}

/* ==================== 主函数 ==================== */

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_think_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_think_d);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    DAEMON_SETUP_SIGNALS(think_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    /* 安全穹顶初始化（与其余 daemon 一致） */
    daemon_cupolas_init("think_d");

    load_daemon_config(config_path);
    if (use_tcp)
        g_config.use_tcp = 1;

    SVC_LOG_INFO("ThinkDual service starting, manager=%s", config_path ? config_path : "default");

    think_service_config_t svc_cfg;
    __builtin_memset(&svc_cfg, 0, sizeof(svc_cfg));
    svc_cfg.enabled = g_config.think_enabled;
    svc_cfg.think2_slow_model = g_config.think2_slow_model[0] ? g_config.think2_slow_model : NULL;
    svc_cfg.think1_fast_model = g_config.think1_fast_model[0] ? g_config.think1_fast_model : NULL;
    svc_cfg.think1_prof_model = g_config.think1_prof_model[0] ? g_config.think1_prof_model : NULL;
    svc_cfg.process_timeout_ms = g_config.process_timeout_ms;

    g_service = think_service_create(&svc_cfg);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create think service");
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_think_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    airy_sock_t server_fd = daemon_create_server_socket(
        g_config.use_tcp, g_config.tcp_port, g_config.socket_path, g_config.socket_path);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_think_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO(g_config.use_tcp ? "Listening on TCP %s:%d" : "Listening on %s",
                 g_config.tcp_host, g_config.tcp_port);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 16;
    ev_config.thread_pool_min = 2;
    ev_config.thread_pool_max = 4;
    ev_config.thread_pool_queue_size = 32;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_think_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = g_config.use_tcp ? g_config.tcp_host : g_config.socket_path;
    int ret = daemon_init_event_driver("think_d", "think", sock_addr,
                                       g_config.use_tcp ? g_config.tcp_port : 0, "think,core",
                                       g_config.use_tcp, &ev_config, &g_event_driver_think_d,
                                       &g_bsd_think_d, &g_bipc_think_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_think_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_think_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_think_d = daemon_event_driver_get_dispatcher(g_event_driver_think_d);
    method_dispatcher_register(g_dispatcher_think_d, "process", on_process_method, NULL);
    method_dispatcher_register(g_dispatcher_think_d, "get_stats", on_get_stats_method, NULL);
    method_dispatcher_register(g_dispatcher_think_d, "health_check", on_health_check_method, NULL);
    /* L2 协议标准方法 <ns>.shutdown（02-l2-service-protocol.md §6.1：优雅停止） */
    method_dispatcher_register(g_dispatcher_think_d, "shutdown", on_shutdown_method_think_d, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (think.* namespace)", 4);

    if (daemon_event_driver_add_server_fd(g_event_driver_think_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_think_d);
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_think_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("ThinkDual service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_think_d);

    daemon_cleanup_standard(g_bipc_think_d, g_bsd_think_d, g_event_driver_think_d,
                            server_fd, destroy_service, &g_running_lock_think_d);
    free_daemon_config();

    SVC_LOG_INFO("ThinkDual service stopped");
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
