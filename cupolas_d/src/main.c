#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file main.c
 * @brief Cupolas 安全穹顶服务守护进程主入口（遵循 daemon 模块统一规范）
 *
 * cupolas_d 将 cupolas 安全库拆分为独立 daemon，暴露 JSON-RPC 方法
 * （cupolas.* 命名空间）：
 *   - cupolas.check_permission : 权限裁决（1 允许 / 0 拒绝）
 *   - cupolas.sanitize         : 输入净化
 *   - cupolas.execute_command  : 隔离工位命令执行
 *   - cupolas.add_rule         : 动态添加权限规则
 *   - cupolas.audit_flush      : 刷新审计日志
 *   - cupolas.get_stats        : 服务统计（真实计数）
 *   - cupolas.shutdown         : 优雅退出
 *
 * cupolas_d 自身是 cupolas 安全库的宿主进程：main() 通过
 * daemon_cupolas_init("cupolas_d") 初始化安全穹顶（权限引擎 + 输入净化 +
 * 审计日志 + daemon_security），退出前 daemon_cupolas_cleanup() 刷新审计。
 *
 * Unix socket 路径：${AIRY_RUNTIME_DIR}/cupolas.sock
 */

#include "daemon_main.h"
#include "platform.h"
#include "param_validator.h"
#include "svc_logger.h"
#include "thread_pool.h"
#include "cupolas_service.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ==================== 配置常量 ==================== */

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("cupolas.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_cupolas"
#define DEFAULT_TCP_PORT 8089
#define MAX_BUFFER 65536
#define MAX_CLIENTS 64

/* 生成公共全局变量、信号处理、help、客户端处理等样板 */
DAEMON_DECLARE_COMMON(cupolas_d, cupolas, DEFAULT_SOCKET_PATH_UNIX,
                      DEFAULT_SOCKET_PATH_WIN, DEFAULT_TCP_PORT, MAX_BUFFER)

/* L2 标准方法 <ns>.shutdown：生成优雅退出处理器（02-l2-service-protocol.md §6.1） */
DAEMON_DECLARE_SHUTDOWN_METHOD(cupolas_d)

/* ==================== 全局状态 ==================== */

static cupolas_service_t *g_service = NULL;

/* 服务配置 */
typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_clients;
} cupolas_daemon_config_t;

static cupolas_daemon_config_t g_config = {0};

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_cupolas_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

/* ==================== 请求处理方法 ==================== */

static void handle_check_permission(cJSON *params, int id, airy_sock_t fd);
static void handle_sanitize(cJSON *params, int id, airy_sock_t fd);
static void handle_execute_command(cJSON *params, int id, airy_sock_t fd);
static void handle_add_rule(cJSON *params, int id, airy_sock_t fd);
static void handle_audit_flush(cJSON *params, int id, airy_sock_t fd);
static void handle_get_stats(int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);

static void on_check_permission_method(cJSON *params, int id, void *user_data)
{
    handle_check_permission(params, id, *(airy_sock_t *)user_data);
}

static void on_sanitize_method(cJSON *params, int id, void *user_data)
{
    handle_sanitize(params, id, *(airy_sock_t *)user_data);
}

static void on_execute_command_method(cJSON *params, int id, void *user_data)
{
    handle_execute_command(params, id, *(airy_sock_t *)user_data);
}

static void on_add_rule_method(cJSON *params, int id, void *user_data)
{
    handle_add_rule(params, id, *(airy_sock_t *)user_data);
}

static void on_audit_flush_method(cJSON *params, int id, void *user_data)
{
    handle_audit_flush(params, id, *(airy_sock_t *)user_data);
}

/* L2 标准方法 cupolas.get_stats（02-l2-service-protocol.md §6.1：真实统计） */
static void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

/* L2 标准方法 cupolas.health_check（02-l2-service-protocol.md） */
static void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
}

/* cupolas.check_permission：权限裁决 */
static void handle_check_permission(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *agent_id = get_string_field(params, "agent_id", NULL);
    const char *action = get_string_field(params, "action", NULL);
    const char *resource = get_string_field(params, "resource", NULL);
    const char *context = get_string_field(params, "context", NULL);

    if (!agent_id || !action || !resource) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "agent_id, action and resource required", id);
        return;
    }

    cupolas_check_permission_params_t req = {.agent_id = agent_id, .action = action,
                                             .resource = resource, .context = context};
    cupolas_check_permission_result_t res = {0};

    int ret = cupolas_service_check_permission(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Permission check failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "allowed", res.allowed ? true : false);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* cupolas.sanitize：输入净化 */
static void handle_sanitize(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *input = get_string_field(params, "input", NULL);
    if (!input) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "input required", id);
        return;
    }

    cupolas_sanitize_params_t req = {.input = input};
    cupolas_sanitize_result_t res = {0};

    int ret = cupolas_service_sanitize(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {
        /* 严格模式下危险输入被拒绝（fail-closed） */
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Input rejected by sanitizer", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "sanitized", res.sanitized ? res.sanitized : "");
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    cupolas_sanitize_result_free(&res);
}

/* cupolas.execute_command：隔离工位命令执行 */
static void handle_execute_command(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *command = get_string_field(params, "command", NULL);
    cJSON *argv_arr = cJSON_GetObjectItem(params, "argv");

    if (!command || !cJSON_IsArray(argv_arr)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "command and argv array required", id);
        return;
    }

    size_t argc = (size_t)cJSON_GetArraySize(argv_arr);
    /* argv[0] = command，后续为参数，末尾 NULL 结尾 */
    char **argv = AIRY_CALLOC(argc + 2, sizeof(char *));
    if (!argv) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    argv[0] = (char *)command;
    for (size_t i = 0; i < argc; i++) {
        cJSON *item = cJSON_GetArrayItem(argv_arr, (int)i);
        if (cJSON_IsString(item))
            argv[i + 1] = item->valuestring;
    }

    cupolas_execute_command_params_t req = {.command = command, .argv = argv};
    cupolas_execute_command_result_t res = {0};

    int ret = cupolas_service_execute_command(g_service, &req, &res);
    AIRY_FREE(argv);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Command execution failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "exit_code", res.exit_code);
    cJSON_AddStringToObject(result, "stdout", res.stdout_buf ? res.stdout_buf : "");
    cJSON_AddStringToObject(result, "stderr", res.stderr_buf ? res.stderr_buf : "");
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    cupolas_execute_command_result_free(&res);
}

/* cupolas.add_rule：动态添加权限规则 */
static void handle_add_rule(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *agent_id = get_string_field(params, "agent_id", NULL);
    const char *action = get_string_field(params, "action", NULL);
    const char *resource = get_string_field(params, "resource", NULL);
    if (!resource) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "resource required", id);
        return;
    }

    /* allow 兼容布尔与 0/1 数字两种表达 */
    int allow = 0;
    cJSON *allow_json = cJSON_GetObjectItem(params, "allow");
    if (cJSON_IsBool(allow_json))
        allow = cJSON_IsTrue(allow_json) ? 1 : 0;
    else if (cJSON_IsNumber(allow_json))
        allow = allow_json->valueint ? 1 : 0;
    int priority = get_int_field(params, "priority", 0);

    cupolas_add_rule_params_t req = {.agent_id = agent_id, .action = action,
                                     .resource = resource, .allow = allow,
                                     .priority = priority};
    cupolas_add_rule_result_t res = {0};

    int ret = cupolas_service_add_rule(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Add rule failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "added", res.added ? true : false);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* cupolas.audit_flush：刷新审计日志 */
static void handle_audit_flush(cJSON *params __attribute__((unused)), int id,
                               airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    int ret = cupolas_service_audit_flush(g_service);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Audit flush failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "flushed", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* L2 标准方法 cupolas.health_check：无副作用健康探针 */
static void handle_health_check(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "cupolas_d");
    cJSON_AddBoolToObject(result, "healthy", g_service != NULL);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* L2 标准方法 cupolas.get_stats（02-l2-service-protocol.md §6.1：真实统计） */
static void handle_get_stats(int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    char *stats_json = cupolas_service_get_stats_json(g_service);
    if (!stats_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Failed to collect stats", id);
        return;
    }

    /* stats_json 为合法 JSON 对象，解析后作为 result 直接透传 */
    cJSON *result = cJSON_Parse(stats_json);
    AIRY_FREE(stats_json);
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Stats serialization failed", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ==================== 配置加载 ==================== */

static int load_daemon_config(const char *config_path)
{
    /* 默认配置 */
    g_config.use_tcp = 0;
    g_config.max_clients = MAX_CLIENTS;

#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_WIN);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#else
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_UNIX);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#endif
    g_config.tcp_port = DEFAULT_TCP_PORT;

    /* 如果提供了配置文件，尝试加载 */
    if (config_path) {
        FILE *f = fopen(config_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);

            if (len > 0 && len < 1024 * 1024) { /* 限制配置文件大小为 1MB */
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
                                if (cJSON_IsNumber(max_clients)) {
                                    g_config.max_clients = max_clients->valueint;
                                }
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

/* ==================== 销毁服务 ==================== */

static void destroy_service(void)
{
    if (g_service) {
        cupolas_service_destroy(g_service);
        g_service = NULL;
    }
}

/* ==================== 主函数 ==================== */

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    /* 解析命令行参数（--manager/--tcp/--help） */
    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_cupolas_d);
    if (parse_rc > 0) return parse_rc == 1 ? 0 : 1;

    /* 初始化平台层 */
    airy_sock_init();
    airy_mtx_init(&g_running_lock_cupolas_d);

    /* 设置信号处理 */
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    DAEMON_SETUP_SIGNALS(cupolas_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    /* cupolas_d 自身是 cupolas 安全库的宿主：
     * 初始化安全穹顶（permission_engine + sanitizer + audit_logger + daemon_security） */
    daemon_cupolas_init("cupolas_d");

    /* 加载配置（命令行 --tcp 覆盖配置文件） */
    load_daemon_config(config_path);
    if (use_tcp)
        g_config.use_tcp = 1;

    SVC_LOG_INFO("Cupolas service starting, manager=%s", config_path ? config_path : "default");

    /* 创建 cupolas 服务（统计 + 配置元数据，模块本体已由 cupolas_init 初始化） */
    g_service = cupolas_service_create(config_path);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create cupolas service");
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_cupolas_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    /* 创建服务器 Socket（TCP/Unix/NamedPipe 统一封装） */
    airy_sock_t server_fd = daemon_create_server_socket(
        g_config.use_tcp, g_config.tcp_port, g_config.socket_path, g_config.socket_path);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_cupolas_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO(g_config.use_tcp ? "Listening on TCP %s:%d" : "Listening on %s",
                 g_config.tcp_host, g_config.tcp_port);

    /* 创建事件驱动 + SD/IPC bootstrap */
    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = 4;
    ev_config.thread_pool_max = 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_cupolas_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = g_config.use_tcp ? g_config.tcp_host : g_config.socket_path;
    int ret = daemon_init_event_driver("cupolas_d", "cupolas", sock_addr,
                                       g_config.use_tcp ? g_config.tcp_port : 0,
                                       "cupolas,security",
                                       g_config.use_tcp, &ev_config, &g_event_driver_cupolas_d,
                                       &g_bsd_cupolas_d, &g_bipc_cupolas_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_cupolas_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_cupolas_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_cupolas_d = daemon_event_driver_get_dispatcher(g_event_driver_cupolas_d);
    method_dispatcher_register(g_dispatcher_cupolas_d, "check_permission",
                               on_check_permission_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "sanitize", on_sanitize_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "execute_command",
                               on_execute_command_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "add_rule", on_add_rule_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "audit_flush", on_audit_flush_method, NULL);
    /* L2 协议标准方法（02-l2-service-protocol.md：cupolas.health_check / cupolas.get_stats /
     * cupolas.shutdown） */
    method_dispatcher_register(g_dispatcher_cupolas_d, "health_check",
                               on_health_check_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "get_stats", on_get_stats_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "shutdown",
                               on_shutdown_method_cupolas_d, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (cupolas.* namespace)", 8);

    if (daemon_event_driver_add_server_fd(g_event_driver_cupolas_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_cupolas_d);
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_cupolas_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("Cupolas service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_cupolas_d);

    /* 标准资源清理链 */
    daemon_cleanup_standard(g_bipc_cupolas_d, g_bsd_cupolas_d, g_event_driver_cupolas_d,
                            server_fd, destroy_service, &g_running_lock_cupolas_d);
    free_daemon_config();

    SVC_LOG_INFO("Cupolas service stopped");
    daemon_cupolas_cleanup(); /* 刷新审计日志并释放安全穹顶资源 */
    log_cleanup();
    return 0;
}
