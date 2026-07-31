#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file main.c
 * @brief A2A 服务守护进程主入口（遵循 daemon 模块统一规范）
 *
 * 暴露 JSON-RPC 方法（a2a.* 命名空间）：
 *   - a2a.register_agent   : 注册智能体
 *   - a2a.unregister_agent : 注销智能体
 *   - a2a.discover_agents  : 发现智能体
 *   - a2a.create_task      : 创建任务
 *   - a2a.update_task      : 更新任务状态
 *   - a2a.cancel_task      : 取消任务
 *   - a2a.get_task         : 获取任务
 *   - a2a.send_message     : 发送消息
 *   - a2a.count            : 返回智能体/任务数（健康检查辅助）
 *
 * Unix socket 路径：${AIRY_RUNTIME_DIR}/a2a.sock
 */

#include "daemon_main.h"
#include "a2a_service.h"
#include "param_validator.h"
#include "svc_logger.h"
#include "thread_pool.h"

#include <stdlib.h>

/* ==================== 配置常量 ==================== */

#define DEFAULT_SOCKET_PATH_UNIX AIRY_RUNTIME_DIR "/a2a.sock"
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_a2a"
#define DEFAULT_TCP_PORT 8087
#define MAX_BUFFER 65536
#define MAX_CLIENTS 64
#define A2A_DEFAULT_MAX_AGENTS 256
#define A2A_DEFAULT_MAX_TASKS 4096

/* 生成公共全局变量、信号处理、help、客户端处理等样板 */
DAEMON_DECLARE_COMMON(a2a_d, a2a, DEFAULT_SOCKET_PATH_UNIX,
                       DEFAULT_SOCKET_PATH_WIN, DEFAULT_TCP_PORT, MAX_BUFFER)

/* ==================== 全局状态 ==================== */

static a2a_service_t *g_service = NULL;

typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_clients;
    size_t max_agents;
    size_t max_tasks;
} a2a_daemon_config_t;

static a2a_daemon_config_t g_config = {0};

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_a2a_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

/* ==================== 请求处理方法 ==================== */

static void handle_register_agent(cJSON *params, int id, airy_sock_t fd);
static void handle_unregister_agent(cJSON *params, int id, airy_sock_t fd);
static void handle_discover(cJSON *params, int id, airy_sock_t fd);
static void handle_create_task(cJSON *params, int id, airy_sock_t fd);
static void handle_update_task(cJSON *params, int id, airy_sock_t fd);
static void handle_cancel_task(cJSON *params, int id, airy_sock_t fd);
static void handle_get_task(cJSON *params, int id, airy_sock_t fd);
static void handle_send_message(cJSON *params, int id, airy_sock_t fd);
static void handle_count(int id, airy_sock_t fd);

static void on_register_agent_method(cJSON *params, int id, void *user_data)
{
    handle_register_agent(params, id, *(airy_sock_t *)user_data);
}

static void on_unregister_agent_method(cJSON *params, int id, void *user_data)
{
    handle_unregister_agent(params, id, *(airy_sock_t *)user_data);
}

static void on_discover_method(cJSON *params, int id, void *user_data)
{
    handle_discover(params, id, *(airy_sock_t *)user_data);
}

static void on_create_task_method(cJSON *params, int id, void *user_data)
{
    handle_create_task(params, id, *(airy_sock_t *)user_data);
}

static void on_update_task_method(cJSON *params, int id, void *user_data)
{
    handle_update_task(params, id, *(airy_sock_t *)user_data);
}

static void on_cancel_task_method(cJSON *params, int id, void *user_data)
{
    handle_cancel_task(params, id, *(airy_sock_t *)user_data);
}

static void on_get_task_method(cJSON *params, int id, void *user_data)
{
    handle_get_task(params, id, *(airy_sock_t *)user_data);
}

static void on_send_message_method(cJSON *params, int id, void *user_data)
{
    handle_send_message(params, id, *(airy_sock_t *)user_data);
}

static void on_count_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_count(id, *(airy_sock_t *)user_data);
}

/* 将 cJSON 值序列化为字符串（调用方负责 free，使用标准 free） */
static char *a2a_cjson_to_string(cJSON *obj)
{
    if (!obj)
        return NULL;
    return cJSON_PrintUnformatted(obj);
}

static void handle_register_agent(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_id = cJSON_GetObjectItem(params, "id");
    cJSON *name = cJSON_GetObjectItem(params, "name");

    if (!agent_id || !cJSON_IsString(agent_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing id string", id);
        return;
    }
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing name string", id);
        return;
    }

    /* 直接转发整个 params 作为 card_json */
    char *card_json = a2a_cjson_to_string(params);
    if (!card_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Card serialize failed", id);
        return;
    }

    int ret = a2a_service_register_agent(g_service, card_json);
    AIRY_FREE(card_json);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Agent register failed", id);
        SVC_LOG_ERROR("a2a.register_agent failed: error=%d", ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "registered", true);
    cJSON_AddStringToObject(result, "agent_id", agent_id->valuestring);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_unregister_agent(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_id = cJSON_GetObjectItem(params, "agent_id");
    if (!agent_id || !cJSON_IsString(agent_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }

    int ret = a2a_service_unregister_agent(g_service, agent_id->valuestring);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Agent not found", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "unregistered", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_discover(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *capability = cJSON_GetObjectItem(params, "capability");
    cJSON *skill = cJSON_GetObjectItem(params, "skill");

    const char *cap_str = (capability && cJSON_IsString(capability))
                              ? capability->valuestring : NULL;
    const char *skill_str = (skill && cJSON_IsString(skill)) ? skill->valuestring : NULL;

    char *results_json = NULL;
    size_t count = 0;
    int ret = a2a_service_discover_agents(g_service, cap_str, skill_str,
                                            &results_json, &count);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Discover failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_Parse(results_json);
    if (arr)
        cJSON_AddItemToObject(result, "agents", arr);
    else
        cJSON_AddItemToObject(result, "agents", cJSON_CreateArray());
    cJSON_AddNumberToObject(result, "count", (double)count);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    a2a_service_results_free(results_json);
}

static void handle_create_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_id = cJSON_GetObjectItem(params, "agent_id");
    cJSON *description = cJSON_GetObjectItem(params, "description");
    cJSON *input = cJSON_GetObjectItem(params, "input");

    if (!agent_id || !cJSON_IsString(agent_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }
    if (!description || !cJSON_IsString(description)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing description", id);
        return;
    }

    const char *input_str = (input && cJSON_IsString(input)) ? input->valuestring : NULL;

    char *task_json = NULL;
    int ret = a2a_service_create_task(g_service, agent_id->valuestring,
                                        description->valuestring, input_str, &task_json);
    if (ret != AIRY_SUCCESS || !task_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Task create failed", id);
        SVC_LOG_ERROR("a2a.create_task failed: error=%d", ret);
        AIRY_FREE(task_json);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *task_obj = cJSON_Parse(task_json);
    if (task_obj)
        cJSON_AddItemToObject(result, "task", task_obj);
    else
        cJSON_AddStringToObject(result, "task", task_json);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    a2a_service_task_free(task_json);
}

static void handle_update_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *task_id = cJSON_GetObjectItem(params, "task_id");
    cJSON *state = cJSON_GetObjectItem(params, "state");
    cJSON *output = cJSON_GetObjectItem(params, "output");
    cJSON *progress = cJSON_GetObjectItem(params, "progress");

    if (!task_id || !cJSON_IsString(task_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing task_id", id);
        return;
    }
    if (!state || !cJSON_IsNumber(state)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing state", id);
        return;
    }

    const char *output_str = (output && cJSON_IsString(output)) ? output->valuestring : NULL;
    double prog = (progress && cJSON_IsNumber(progress)) ? progress->valuedouble : 0.0;

    int ret = a2a_service_update_task(g_service, task_id->valuestring,
                                        state->valueint, output_str, prog);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Task not found", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "updated", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_cancel_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *task_id = cJSON_GetObjectItem(params, "task_id");
    cJSON *reason = cJSON_GetObjectItem(params, "reason");

    if (!task_id || !cJSON_IsString(task_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing task_id", id);
        return;
    }

    const char *reason_str = (reason && cJSON_IsString(reason)) ? reason->valuestring : NULL;

    int ret = a2a_service_cancel_task(g_service, task_id->valuestring, reason_str);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Task not found", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "canceled", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_get_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *task_id = cJSON_GetObjectItem(params, "task_id");
    if (!task_id || !cJSON_IsString(task_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing task_id", id);
        return;
    }

    char *task_json = NULL;
    int ret = a2a_service_get_task(g_service, task_id->valuestring, &task_json);
    if (ret != AIRY_SUCCESS || !task_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Task not found", id);
        AIRY_FREE(task_json);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *task_obj = cJSON_Parse(task_json);
    if (task_obj)
        cJSON_AddItemToObject(result, "task", task_obj);
    else
        cJSON_AddStringToObject(result, "task", task_json);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    a2a_service_task_free(task_json);
}

static void handle_send_message(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *target = cJSON_GetObjectItem(params, "target_agent_id");
    cJSON *role = cJSON_GetObjectItem(params, "role");
    cJSON *content = cJSON_GetObjectItem(params, "content");

    if (!target || !cJSON_IsString(target)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing target_agent_id", id);
        return;
    }
    if (!role || !cJSON_IsString(role)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing role", id);
        return;
    }
    if (!content || !cJSON_IsString(content)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing content", id);
        return;
    }

    char *response_json = NULL;
    size_t response_count = 0;
    int ret = a2a_service_send_message(g_service, target->valuestring,
                                         role->valuestring, content->valuestring,
                                         &response_json, &response_count);
    if (ret != AIRY_SUCCESS || !response_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Send message failed", id);
        SVC_LOG_ERROR("a2a.send_message failed: error=%d", ret);
        a2a_service_results_free(response_json);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_Parse(response_json);
    if (arr)
        cJSON_AddItemToObject(result, "responses", arr);
    else
        cJSON_AddItemToObject(result, "responses", cJSON_CreateArray());
    cJSON_AddNumberToObject(result, "count", (double)response_count);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    a2a_service_results_free(response_json);
}

static void handle_count(int id, airy_sock_t client_fd)
{
    size_t agent_count = a2a_service_count(g_service);
    size_t task_count = a2a_service_task_count(g_service);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "agent_count", (double)agent_count);
    cJSON_AddNumberToObject(result, "task_count", (double)task_count);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ==================== 配置加载 ==================== */

static int load_daemon_config(const char *config_path)
{
    g_config.use_tcp = 0;
    g_config.max_clients = MAX_CLIENTS;
    g_config.max_agents = A2A_DEFAULT_MAX_AGENTS;
    g_config.max_tasks = A2A_DEFAULT_MAX_TASKS;

#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_WIN);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#else
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_UNIX);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#endif
    g_config.tcp_port = DEFAULT_TCP_PORT;

    /* 环境变量覆盖：AIRY_A2A_MAX_AGENTS / AIRY_A2A_MAX_TASKS */
    const char *env_agents = getenv("AIRY_A2A_MAX_AGENTS");
    if (env_agents) {
        unsigned long v = strtoul(env_agents, NULL, 10);
        if (v > 0 && v < 65536)
            g_config.max_agents = (size_t)v;
    }
    const char *env_tasks = getenv("AIRY_A2A_MAX_TASKS");
    if (env_tasks) {
        unsigned long v = strtoul(env_tasks, NULL, 10);
        if (v > 0 && v < 1048576)
            g_config.max_tasks = (size_t)v;
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
                                cJSON *max_tasks = cJSON_GetObjectItem(daemon_cfg, "max_tasks");
                                if (cJSON_IsNumber(max_tasks))
                                    g_config.max_tasks = (size_t)max_tasks->valuedouble;
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
        a2a_service_destroy(g_service);
        g_service = NULL;
    }
}

/* ==================== 主函数 ==================== */

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_a2a_d);
    if (parse_rc > 0) return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_a2a_d);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    DAEMON_SETUP_SIGNALS(a2a_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    /* P3.14 ACC-DT15: 初始化 cupolas 安全穹顶 */
    daemon_cupolas_init("a2a_d");

    load_daemon_config(config_path);
    if (use_tcp)
        g_config.use_tcp = 1;

    SVC_LOG_INFO("A2A service starting, manager=%s", config_path ? config_path : "default");

    g_service = a2a_service_create(g_config.max_agents, g_config.max_tasks);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create a2a service");
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_a2a_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    airy_sock_t server_fd = daemon_create_server_socket(
        g_config.use_tcp, g_config.tcp_port, g_config.socket_path, g_config.socket_path);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_a2a_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO(g_config.use_tcp ? "Listening on TCP %s:%d" : "Listening on %s",
                 g_config.tcp_host, g_config.tcp_port);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = 4;
    ev_config.thread_pool_max = 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_a2a_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = g_config.use_tcp ? g_config.tcp_host : g_config.socket_path;
    int ret = daemon_init_event_driver("a2a_d", "a2a", sock_addr,
                                         g_config.use_tcp ? g_config.tcp_port : 0, "a2a,core",
                                         g_config.use_tcp, &ev_config, &g_event_driver_a2a_d,
                                         &g_bsd_a2a_d, &g_bipc_a2a_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_a2a_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_a2a_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_a2a_d = daemon_event_driver_get_dispatcher(g_event_driver_a2a_d);
    method_dispatcher_register(g_dispatcher_a2a_d, "register_agent", on_register_agent_method, NULL);
    method_dispatcher_register(g_dispatcher_a2a_d, "unregister_agent", on_unregister_agent_method, NULL);
    method_dispatcher_register(g_dispatcher_a2a_d, "discover_agents", on_discover_method, NULL);
    method_dispatcher_register(g_dispatcher_a2a_d, "create_task", on_create_task_method, NULL);
    method_dispatcher_register(g_dispatcher_a2a_d, "update_task", on_update_task_method, NULL);
    method_dispatcher_register(g_dispatcher_a2a_d, "cancel_task", on_cancel_task_method, NULL);
    method_dispatcher_register(g_dispatcher_a2a_d, "get_task", on_get_task_method, NULL);
    method_dispatcher_register(g_dispatcher_a2a_d, "send_message", on_send_message_method, NULL);
    method_dispatcher_register(g_dispatcher_a2a_d, "count", on_count_method, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (a2a.* namespace)", 9);

    if (daemon_event_driver_add_server_fd(g_event_driver_a2a_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_a2a_d);
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_a2a_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("A2A service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_a2a_d);

    daemon_cleanup_standard(g_bipc_a2a_d, g_bsd_a2a_d, g_event_driver_a2a_d,
                             server_fd, destroy_service, &g_running_lock_a2a_d);
    free_daemon_config();

    SVC_LOG_INFO("A2A service stopped");
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
