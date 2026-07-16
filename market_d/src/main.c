#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file main.c
 * @brief 市场服务守护进程主入口（遵循 daemon 模块统一规范）
 */

#include "daemon_main.h"
#include "market_service.h"
#include "param_validator.h"
#include "svc_logger.h"
#include "thread_pool.h"

#include <stdlib.h>
#include <time.h>

/* ==================== 配置常量 ==================== */

#define DEFAULT_SOCKET_PATH_UNIX AIRY_RUNTIME_DIR "/market.sock"
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_market"
#define DEFAULT_TCP_PORT 8082
#define MAX_BUFFER 65536

/* 生成公共全局变量、信号处理、help、客户端处理等样板 */
DAEMON_DECLARE_COMMON(market_d, market, DEFAULT_SOCKET_PATH_UNIX,
                      DEFAULT_SOCKET_PATH_WIN, DEFAULT_TCP_PORT, MAX_BUFFER)

/* ==================== 全局状态 ==================== */

static market_service_t *g_service = NULL;

/* ==================== 错误码定义 ==================== */
#define MARKET_ERR_INVALID_PARAM AIRY_ERR_INVALID_PARAM
#define MARKET_ERR_OUT_OF_MEMORY AIRY_ERR_OUT_OF_MEMORY
#define MARKET_ERR_NOT_FOUND AIRY_ERR_NOT_FOUND
#define MARKET_ERR_ALREADY_EXISTS (AIRY_ERR_DAEMON_BASE + 0x20)
#define MARKET_ERR_INSTALL_FAIL (AIRY_ERR_DAEMON_BASE + 0x21)

/* ==================== 方法处理器包装函数 ==================== */

static void handle_register_agent(cJSON *params, int id, airy_sock_t client_fd);
static void handle_search_agents(cJSON *params, int id, airy_sock_t client_fd);
static void handle_install_agent(cJSON *params, int id, airy_sock_t client_fd);
static void handle_register_skill(cJSON *params, int id, airy_sock_t client_fd);
static void handle_search_skills(cJSON *params, int id, airy_sock_t client_fd);
static void handle_health_check(int id, airy_sock_t client_fd);

static void on_register_agent_method(cJSON *params, int id, void *user_data)
{
    handle_register_agent(params, id, *(airy_sock_t *)user_data);
}

static void on_search_agents_method(cJSON *params, int id, void *user_data)
{
    handle_search_agents(params, id, *(airy_sock_t *)user_data);
}

static void on_install_agent_method(cJSON *params, int id, void *user_data)
{
    handle_install_agent(params, id, *(airy_sock_t *)user_data);
}

static void on_register_skill_method(cJSON *params, int id, void *user_data)
{
    handle_register_skill(params, id, *(airy_sock_t *)user_data);
}

static void on_search_skills_method(cJSON *params, int id, void *user_data)
{
    handle_search_skills(params, id, *(airy_sock_t *)user_data);
}

static void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
}

static int register_rpc_methods(void)
{
    g_dispatcher_market_d = method_dispatcher_create(16);
    if (!g_dispatcher_market_d) {
        SVC_LOG_ERROR("Failed to create method dispatcher");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    method_dispatcher_register(g_dispatcher_market_d, "register_agent", on_register_agent_method, NULL);
    method_dispatcher_register(g_dispatcher_market_d, "search_agents", on_search_agents_method, NULL);
    method_dispatcher_register(g_dispatcher_market_d, "install_agent", on_install_agent_method, NULL);
    method_dispatcher_register(g_dispatcher_market_d, "register_skill", on_register_skill_method, NULL);
    method_dispatcher_register(g_dispatcher_market_d, "search_skills", on_search_skills_method, NULL);
    method_dispatcher_register(g_dispatcher_market_d, "health_check", on_health_check_method, NULL);

    SVC_LOG_INFO("Registered %d RPC methods", 6);
    return 0;
}

/* 线程池提交回调：复用生成的 daemon_handle_client_market_d */
static void handle_client_wrapper(void *arg)
{
    daemon_handle_client_market_d((airy_sock_t)(uintptr_t)arg, g_dispatcher_market_d);
}

static void handle_register_agent(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_json = jsonrpc_get_object_param(params, "agent");
    if (!agent_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent object", id);
        return;
    }

    agent_info_t info = {0};
    const char *aid = get_string_field(agent_json, "agent_id", NULL);
    if (!aid) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }
    info.agent_id = (char *)aid;

    info.name = (char *)get_string_field(agent_json, "name", NULL);
    info.version = (char *)get_string_field(agent_json, "version", NULL);
    info.description = (char *)get_string_field(agent_json, "description", NULL);
    info.author = (char *)get_string_field(agent_json, "author", NULL);

    int ret = market_service_register_agent(g_service, &info);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Register failed", id);
        SVC_LOG_ERROR("Failed to register agent: %s (error=%d)", aid, ret);
    } else {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "registered");
        cJSON_AddStringToObject(result, "agent_id", aid);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_INFO("Agent registered: %s v%s", aid, info.version ? info.version : "unknown");
    }
}

static void handle_search_agents(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *keyword = get_string_field(params, "keyword", "");
    size_t offset = (size_t)get_double_field(params, "offset", 0.0);
    size_t limit = (size_t)get_double_field(params, "limit", 20.0);

    agent_info_t **agents = NULL;
    size_t count = 0;

    search_params_t sp;
    __builtin_memset(&sp, 0, sizeof(sp));
    sp.query = (char *)keyword;
    sp.limit = limit;
    sp.offset = offset;

    int ret = market_service_search_agents(g_service, &sp, &agents, &count);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Search failed", id);
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count && agents && agents[i]; i++) {
        cJSON *a = cJSON_CreateObject();
        if (agents[i]->agent_id)
            cJSON_AddStringToObject(a, "agent_id", agents[i]->agent_id);
        if (agents[i]->name)
            cJSON_AddStringToObject(a, "name", agents[i]->name);
        if (agents[i]->version)
            cJSON_AddStringToObject(a, "version", agents[i]->version);
        if (agents[i]->description)
            cJSON_AddStringToObject(a, "description", agents[i]->description);
        if (agents[i]->author)
            cJSON_AddStringToObject(a, "author", agents[i]->author);
        cJSON_AddBoolToObject(a, "installed", agents[i]->status == AGENT_STATUS_AVAILABLE);
        cJSON_AddItemToArray(arr, a);
    }
    AIRY_FREE(agents);

    JSONRPC_SEND_SUCCESS(client_fd, arr, id);
}

static void handle_install_agent(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *aid = get_string_field(params, "agent_id", NULL);
    if (!aid) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }

    const char *version = get_string_field(params, "version", "latest");

    int ret = market_service_install_agent(g_service, (const install_request_t *)aid,
                                           (install_result_t **)version);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Install failed", id);
        SVC_LOG_ERROR("Failed to install agent: %s@%s (error=%d)", aid, version, ret);
    } else {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "installed");
        cJSON_AddStringToObject(result, "agent_id", aid);
        cJSON_AddStringToObject(result, "installed_version", version);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_INFO("Agent installed: %s@%s", aid, version);
    }
}

static void handle_register_skill(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *skill_json = jsonrpc_get_object_param(params, "skill");
    if (!skill_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing skill object", id);
        return;
    }

    skill_info_t info = {0};
    const char *sid = get_string_field(skill_json, "skill_id", NULL);
    if (!sid) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing skill_id", id);
        return;
    }
    info.skill_id = (char *)sid;

    info.name = (char *)get_string_field(skill_json, "name", NULL);
    info.version = (char *)get_string_field(skill_json, "version", NULL);

    int ret = market_service_register_skill(g_service, &info);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Register failed", id);
        SVC_LOG_ERROR("Failed to register skill: %s (error=%d)", sid, ret);
    } else {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "registered");
        cJSON_AddStringToObject(result, "skill_id", sid);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_INFO("Skill registered: %s", sid);
    }
}

static void handle_search_skills(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *keyword = get_string_field(params, "keyword", "");

    skill_info_t **skills = NULL;
    size_t count = 0;

    search_params_t sp;
    __builtin_memset(&sp, 0, sizeof(sp));
    sp.query = (char *)keyword;
    sp.limit = 20;
    sp.offset = 0;

    int ret = market_service_search_skills(g_service, &sp, &skills, &count);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Search failed", id);
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count && skills && skills[i]; i++) {
        cJSON *s = cJSON_CreateObject();
        if (skills[i]->skill_id)
            cJSON_AddStringToObject(s, "skill_id", skills[i]->skill_id);
        if (skills[i]->name)
            cJSON_AddStringToObject(s, "name", skills[i]->name);
        if (skills[i]->version)
            cJSON_AddStringToObject(s, "version", skills[i]->version);
        if (skills[i]->description)
            cJSON_AddStringToObject(s, "description", skills[i]->description);
        cJSON_AddItemToArray(arr, s);
    }
    AIRY_FREE(skills);

    JSONRPC_SEND_SUCCESS(client_fd, arr, id);
}

static void handle_health_check(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "market_d");
    cJSON_AddBoolToObject(result, "healthy", true);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ==================== 销毁服务 ==================== */

static void destroy_service(void)
{
    if (g_service) {
        market_service_destroy(g_service);
        g_service = NULL;
    }
}

/* ==================== 主函数 ==================== */

int main(int argc, char **argv)
{
    const char *config_path = "agentrt/manager/service/market_d/market.yaml";
    int use_tcp = 0;

    /* 解析命令行参数（--manager/--tcp/--help） */
    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_market_d);
    if (parse_rc > 0) return parse_rc == 1 ? 0 : 1;

    /* 初始化平台层 */
    airy_sock_init();
    airy_mtx_init(&g_running_lock_market_d);

    /* 设置信号处理 */
#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler_market_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(market_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    /* P3.14 ACC-DT15: 初始化 cupolas 安全穹顶（permission_engine + sanitizer + audit_logger）*/
    daemon_cupolas_init("market_d");

    SVC_LOG_INFO("Market service starting, manager=%s", config_path);

    /* 创建配置 */
    market_config_t config = {.registry_url = NULL,
                              .storage_path = "market.log",
                              .sync_interval_ms = 30000,
                              .cache_ttl_ms = 3600000,
                              .enable_remote_registry = false,
                              .enable_auto_update = false};

    /* 创建市场服务 */
    int ret = market_service_create(&config, &g_service);
    if (ret != AIRY_SUCCESS || !g_service) {
        SVC_LOG_ERROR("Failed to create market service (error=%d)", ret);
        airy_mtx_destroy(&g_running_lock_market_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    /* 注册 RPC 方法 */
    if (register_rpc_methods() != 0) {
        SVC_LOG_ERROR("Failed to register RPC methods");
        destroy_service();
        airy_mtx_destroy(&g_running_lock_market_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("Market service created successfully");

    /* 创建服务器 Socket（TCP/Unix/NamedPipe 统一封装） */
    airy_sock_t server_fd = daemon_create_server_socket(
        use_tcp, DEFAULT_TCP_PORT, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        method_dispatcher_destroy(g_dispatcher_market_d);
        destroy_service();
        airy_mtx_destroy(&g_running_lock_market_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO(use_tcp ? "Listening on TCP port %d" : "Listening on Unix socket",
                 DEFAULT_TCP_PORT);

    /* SD/IPC bootstrap */
    const char *sock_addr = use_tcp ? "127.0.0.1" : DEFAULT_SOCKET_PATH_UNIX;
    g_bsd_market_d = daemon_bootstrap_sd_start("market_d", "market", sock_addr,
                                                use_tcp ? DEFAULT_TCP_PORT : 0, "market,core", 0);
    g_bipc_market_d = daemon_bootstrap_ipc_start("market_d", "market", sock_addr,
                                                  use_tcp ? DEFAULT_TCP_PORT : 0,
                                                  IPC_BUS_PROTO_JSON_RPC);

    SVC_LOG_INFO("Market service started successfully");

    thread_pool_config_t tp_config;
    tp_config.min_threads = 4;
    tp_config.max_threads = 8;
    tp_config.queue_size = 256;
    tp_config.idle_timeout_ms = 30000;
    thread_pool_t *pool = thread_pool_create(&tp_config);
    if (!pool) {
        SVC_LOG_ERROR("Failed to create thread pool");
        daemon_bootstrap_ipc_stop(g_bipc_market_d);
        daemon_bootstrap_sd_stop(g_bsd_market_d);
        airy_sock_close(server_fd);
        method_dispatcher_destroy(g_dispatcher_market_d);
        destroy_service();
        airy_mtx_destroy(&g_running_lock_market_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    /* 主事件循环：接受连接并提交线程池并发处理 */
    while (atomic_load_explicit(&g_running_market_d, memory_order_acquire)) {
        airy_sock_t client_fd = airy_sock_accept(server_fd, 5000);
        if (client_fd == AIRY_INVALID_SOCKET)
            continue;

        thread_pool_submit(pool, handle_client_wrapper, (void *)(uintptr_t)client_fd);
    }

    /* 清理资源 */
    daemon_cleanup_standard(g_bipc_market_d, g_bsd_market_d, NULL, server_fd,
                           destroy_service, &g_running_lock_market_d);
    thread_pool_destroy(pool);
    if (g_dispatcher_market_d)
        method_dispatcher_destroy(g_dispatcher_market_d);

    SVC_LOG_INFO("Market service stopped");
    daemon_cupolas_cleanup(); /* P3.14 ACC-DT15: 清理 cupolas 安全穹顶 */
    log_cleanup();
    return 0;
}
