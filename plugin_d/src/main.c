// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file main.c
 * @brief Plugin 守护进程入口 — P2.2 完整实现（事件驱动模式）
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * 启动流程：SD 注册 → IPC 路由 → 插件发现 → 权限校验 → 扫描加载 → 事件驱动 accept
 *
 * 暴露 JSON-RPC 方法（plugin.* 命名空间）：
 *   - plugin.load          : 从动态库加载插件
 *   - plugin.unload        : 卸载插件
 *   - plugin.start         : 启动插件
 *   - plugin.stop          : 停止插件
 *   - plugin.execute       : 执行插件（调用插件导出的 plugin_execute，JSON 入参→出参）
 *   - plugin.get_metadata  : 获取插件元数据
 *   - plugin.get_state     : 获取插件状态
 *   - plugin.get_stats     : 获取插件统计
 *   - plugin.list          : 列出已加载插件
 *
 * 修复历史：原实现事件循环为 while(g_running) sleep(1)，从不 accept、
 * 不注册 RPC 方法（僵尸服务）。重写为 daemon_event_driver 事件驱动，
 * 与 mem_d/agent_d 等标准 daemon 保持一致。
 */

#include "airy_memory.h"
#include "daemon_main.h"
#include "platform.h"
#include "plugin_discovery.h"
#include "plugin_permission.h"
#include "plugin_service.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ==================== 配置常量 ==================== */

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("plugin.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_plugin"
#define DEFAULT_TCP_PORT 8092
#define MAX_BUFFER 65536
#define MAX_CLIENTS 64

/* 生成公共全局变量、信号处理、help、客户端处理等样板 */
DAEMON_DECLARE_COMMON(plugin_d, plugin, DEFAULT_SOCKET_PATH_UNIX,
                       DEFAULT_SOCKET_PATH_WIN, DEFAULT_TCP_PORT, MAX_BUFFER)

/* L2 标准方法 <ns>.shutdown：生成优雅退出处理器（02-l2-service-protocol.md §6.1） */
DAEMON_DECLARE_SHUTDOWN_METHOD(plugin_d)

/* ==================== 全局状态 ==================== */

static plugin_discovery_result_t *g_discovered = NULL;
static size_t g_discovered_count = 0;

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_plugin_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

/* ==================== 请求处理方法 ==================== */

static void handle_load(cJSON *params, int id, airy_sock_t fd);
static void handle_unload(cJSON *params, int id, airy_sock_t fd);
static void handle_start(cJSON *params, int id, airy_sock_t fd);
static void handle_stop(cJSON *params, int id, airy_sock_t fd);
static void handle_execute(cJSON *params, int id, airy_sock_t fd);
static void handle_get_metadata(cJSON *params, int id, airy_sock_t fd);
static void handle_get_state(cJSON *params, int id, airy_sock_t fd);
static void handle_get_stats(cJSON *params, int id, airy_sock_t fd);
static void handle_list(cJSON *params, int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);

static void on_load_method(cJSON *params, int id, void *user_data)
{
    handle_load(params, id, *(airy_sock_t *)user_data);
}

static void on_unload_method(cJSON *params, int id, void *user_data)
{
    handle_unload(params, id, *(airy_sock_t *)user_data);
}

static void on_start_method(cJSON *params, int id, void *user_data)
{
    handle_start(params, id, *(airy_sock_t *)user_data);
}

static void on_stop_method(cJSON *params, int id, void *user_data)
{
    handle_stop(params, id, *(airy_sock_t *)user_data);
}

static void on_execute_method(cJSON *params, int id, void *user_data)
{
    handle_execute(params, id, *(airy_sock_t *)user_data);
}

static void on_get_metadata_method(cJSON *params, int id, void *user_data)
{
    handle_get_metadata(params, id, *(airy_sock_t *)user_data);
}

static void on_get_state_method(cJSON *params, int id, void *user_data)
{
    handle_get_state(params, id, *(airy_sock_t *)user_data);
}

static void on_get_stats_method(cJSON *params, int id, void *user_data)
{
    handle_get_stats(params, id, *(airy_sock_t *)user_data);
}

static void on_list_method(cJSON *params, int id, void *user_data)
{
    handle_list(params, id, *(airy_sock_t *)user_data);
}

/* L2 标准方法 plugin.health_check（02-l2-service-protocol.md） */
static void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
}

static void handle_load(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *library_path = cJSON_GetObjectItem(params, "library_path");
    cJSON *config_path = cJSON_GetObjectItem(params, "config_path");

    if (!library_path || !cJSON_IsString(library_path)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing library_path string", id);
        return;
    }

    const char *out_name = NULL;
    int ret = plugin_service_load(library_path->valuestring,
                                  config_path && cJSON_IsString(config_path)
                                      ? config_path->valuestring
                                      : NULL,
                                  &out_name);
    if (ret != 0 || !out_name) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Plugin load failed", id);
        SVC_LOG_ERROR("plugin.load failed: library=%s error=%d",
                      library_path->valuestring, ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "name", out_name);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("plugin.load OK: name=%s library=%s", out_name,
                 library_path->valuestring);
}

static void handle_unload(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing name string", id);
        return;
    }

    int ret = plugin_service_unload(name->valuestring);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Plugin unload failed", id);
        SVC_LOG_ERROR("plugin.unload failed: name=%s error=%d",
                      name->valuestring, ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "unloaded", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("plugin.unload OK: name=%s", name->valuestring);
}

static void handle_start(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing name string", id);
        return;
    }

    int ret = plugin_service_start(name->valuestring);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Plugin start failed", id);
        SVC_LOG_ERROR("plugin.start failed: name=%s error=%d",
                      name->valuestring, ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "started", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("plugin.start OK: name=%s", name->valuestring);
}

static void handle_stop(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing name string", id);
        return;
    }

    int ret = plugin_service_stop(name->valuestring);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Plugin stop failed", id);
        SVC_LOG_ERROR("plugin.stop failed: name=%s error=%d",
                      name->valuestring, ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "stopped", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("plugin.stop OK: name=%s", name->valuestring);
}

static void handle_execute(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *name = cJSON_GetObjectItem(params, "name");
    cJSON *input = cJSON_GetObjectItem(params, "input");
    if (!name || !cJSON_IsString(name) || !input || !cJSON_IsString(input)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing name/input strings", id);
        return;
    }

    char *output = NULL;
    int ret = plugin_service_execute(name->valuestring, input->valuestring,
                                     &output);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Plugin execute failed", id);
        SVC_LOG_ERROR("plugin.execute failed: name=%s error=%d",
                      name->valuestring, ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "output", output ? output : "");
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("plugin.execute OK: name=%s output_len=%zu",
                 name->valuestring, output ? strlen(output) : 0);

    if (output) AIRY_FREE(output);
}

static void handle_get_metadata(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing name string", id);
        return;
    }

    plugin_metadata_t metadata;
    __builtin_memset(&metadata, 0, sizeof(metadata));
    int ret = plugin_service_get_metadata(name->valuestring, &metadata);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND,
                           "Plugin not found", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "name", metadata.name);
    cJSON_AddStringToObject(result, "version", metadata.version);
    cJSON_AddStringToObject(result, "author", metadata.author);
    cJSON_AddStringToObject(result, "description", metadata.description);
    cJSON_AddNumberToObject(result, "type", (double)metadata.type);
    cJSON_AddNumberToObject(result, "api_version", (double)metadata.api_version);
    cJSON_AddNumberToObject(result, "min_airy_version",
                            (double)metadata.min_airy_version);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_get_state(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing name string", id);
        return;
    }

    plugin_state_t state = plugin_service_get_state(name->valuestring);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "state", (double)state);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_get_stats(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing name string", id);
        return;
    }

    plugin_stats_t stats;
    __builtin_memset(&stats, 0, sizeof(stats));
    int ret = plugin_service_get_stats(name->valuestring, &stats);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND,
                           "Plugin not found", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "load_count", (double)stats.load_count);
    cJSON_AddNumberToObject(result, "error_count", (double)stats.error_count);
    cJSON_AddNumberToObject(result, "uptime_ns", (double)stats.uptime_ns);
    cJSON_AddNumberToObject(result, "memory_bytes", (double)stats.memory_bytes);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_list(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *type_filter = cJSON_GetObjectItem(params, "type_filter");
    int filter = type_filter && cJSON_IsNumber(type_filter)
                     ? type_filter->valueint
                     : -1;

    char **names = NULL;
    size_t count = 0;
    int ret = plugin_service_list(&names, &count, filter);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Plugin list failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        if (names[i])
            cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    }
    cJSON_AddItemToObject(result, "plugins", arr);
    cJSON_AddNumberToObject(result, "total", (double)count);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);

    if (names) {
        for (size_t i = 0; i < count; i++)
            AIRY_FREE(names[i]);
        AIRY_FREE(names);
    }
}

/* L2 标准方法 plugin.health_check：无副作用健康探针（含已加载插件数） */
static void handle_health_check(int id, airy_sock_t client_fd)
{
    bool healthy = true;
    size_t plugin_count = 0;

    char **names = NULL;
    int ret = plugin_service_list(&names, &plugin_count, -1);
    if (ret != 0)
        healthy = false;
    if (names) {
        for (size_t i = 0; i < plugin_count; i++)
            AIRY_FREE(names[i]);
        AIRY_FREE(names);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "plugin_d");
    cJSON_AddBoolToObject(result, "healthy", healthy);
    cJSON_AddNumberToObject(result, "plugin_count", (double)plugin_count);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ==================== 插件发现与加载 ==================== */

/* 启动时扫描插件目录、权限校验并加载（原实现逻辑保留） */
static void scan_and_load_plugins(void)
{
    plugin_discovery_scan(&g_discovered, &g_discovered_count);
    if (g_discovered_count == 0) {
        SVC_LOG_INFO("Plugin_d: no plugins found");
        return;
    }

    SVC_LOG_INFO("Plugin_d: found %zu plugin(s)", g_discovered_count);
    size_t loaded = 0;
    for (size_t i = 0; i < g_discovered_count; i++) {
        if (!g_discovered[i].valid) {
            SVC_LOG_WARN("Plugin_d: '%s' invalid: %s", g_discovered[i].name,
                         g_discovered[i].error_reason);
            continue;
        }

        /* 权限校验 */
        char denied[512] = {0};
        plugin_permission_result_t perm_result = plugin_permission_check(
            (const char (*)[64])g_discovered[i].permissions,
            g_discovered[i].permission_count,
            g_discovered[i].name, denied, sizeof(denied));

        if (perm_result != PLUGIN_PERM_ALLOWED) {
            SVC_LOG_WARN("Plugin_d: skipping '%s' — permission denied: %s",
                         g_discovered[i].name, denied);
            continue;
        }

        /* 加载并启动插件 */
        const char *out_name = NULL;
        int load_ret = plugin_service_load(
            g_discovered[i].library_path, NULL, &out_name);
        if (load_ret == 0) {
            plugin_service_start(g_discovered[i].name);
            loaded++;
            SVC_LOG_INFO("Plugin_d: loaded and started '%s'",
                         g_discovered[i].name);
        } else {
            SVC_LOG_ERROR("Plugin_d: failed to load '%s' from '%s' (error=%d)",
                          g_discovered[i].name, g_discovered[i].library_path,
                          load_ret);
        }
    }
    SVC_LOG_INFO("Plugin_d: loaded %zu/%zu plugin(s)", loaded,
                 g_discovered_count);
}

/* ==================== 销毁回调 ==================== */

static void destroy_service(void)
{
    plugin_discovery_free_results(g_discovered, g_discovered_count);
    g_discovered = NULL;
    g_discovered_count = 0;
    plugin_discovery_destroy();
    plugin_permission_destroy();
    daemon_cupolas_cleanup();
}

/* ==================== 主函数 ==================== */

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp,
                                     print_usage_plugin_d);
    if (parse_rc > 0) return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_plugin_d);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    DAEMON_SETUP_SIGNALS(plugin_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    /* P3.14 ACC-DT15: 初始化 cupolas 安全穹顶 */
    daemon_cupolas_init("plugin_d");

    /* P2.2.4: 初始化权限校验模块 */
    plugin_permission_config_t perm_cfg;
    __builtin_memset(&perm_cfg, 0, sizeof(perm_cfg));
    perm_cfg.enable_strict_mode = true;
    perm_cfg.enable_audit_log = true;
    perm_cfg.agent_id = "plugin_d";
    if (plugin_permission_init(&perm_cfg) != 0) {
        SVC_LOG_WARN("Plugin_d: permission module init failed");
    }

    /* P2.2.1: 初始化插件发现模块
     * plugins_dir 必须为绝对路径：$AIRY_HOME/ecosystem/plugins。
     * 历史上硬编码 "ecosystem/plugins/" 相对路径，随进程 CWD 漂移，
     * daemon 从任意目录启动都会扫描失败（插件列表为空）。 */
    char plugins_dir[AIRY_PATH_MAX];
    snprintf(plugins_dir, sizeof(plugins_dir), "%s/ecosystem/plugins",
             airy_home_dir());
    plugin_discovery_config_t disc_cfg;
    __builtin_memset(&disc_cfg, 0, sizeof(disc_cfg));
    disc_cfg.plugins_dir = plugins_dir;
    disc_cfg.auto_load = false;
    disc_cfg.fail_on_invalid = false;
    disc_cfg.scan_depth = 1;
    if (plugin_discovery_init(&disc_cfg) != 0) {
        SVC_LOG_ERROR("Plugin_d: plugin discovery init failed");
    }

    /* 启动时扫描并加载插件 */
    scan_and_load_plugins();

    SVC_LOG_INFO("Plugin_d: creating server socket (config=%s)",
                 config_path ? config_path : "default");

    airy_sock_t server_fd = daemon_create_server_socket(
        use_tcp, DEFAULT_TCP_PORT, DEFAULT_SOCKET_PATH_UNIX,
        DEFAULT_SOCKET_PATH_WIN);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Plugin_d: failed to create server socket");
        destroy_service();
        airy_mtx_destroy(&g_running_lock_plugin_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO("Plugin_d: listening on %s (fd=%d)",
                 use_tcp ? "TCP" : DEFAULT_SOCKET_PATH_UNIX, (int)server_fd);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = 4;
    ev_config.thread_pool_max = 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_plugin_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = use_tcp ? "127.0.0.1" : DEFAULT_SOCKET_PATH_UNIX;
    int ret = daemon_init_event_driver("plugin_d", "plugin", sock_addr,
                                        use_tcp ? DEFAULT_TCP_PORT : 0,
                                        "plugin,core", use_tcp, &ev_config,
                                        &g_event_driver_plugin_d,
                                        &g_bsd_plugin_d, &g_bipc_plugin_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_plugin_d) {
        SVC_LOG_ERROR("Plugin_d: failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service();
        airy_mtx_destroy(&g_running_lock_plugin_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_plugin_d = daemon_event_driver_get_dispatcher(g_event_driver_plugin_d);
    method_dispatcher_register(g_dispatcher_plugin_d, "load", on_load_method, NULL);
    method_dispatcher_register(g_dispatcher_plugin_d, "unload", on_unload_method, NULL);
    method_dispatcher_register(g_dispatcher_plugin_d, "start", on_start_method, NULL);
    method_dispatcher_register(g_dispatcher_plugin_d, "stop", on_stop_method, NULL);
    method_dispatcher_register(g_dispatcher_plugin_d, "execute", on_execute_method, NULL);
    method_dispatcher_register(g_dispatcher_plugin_d, "get_metadata", on_get_metadata_method, NULL);
    method_dispatcher_register(g_dispatcher_plugin_d, "get_state", on_get_state_method, NULL);
    method_dispatcher_register(g_dispatcher_plugin_d, "get_stats", on_get_stats_method, NULL);
    method_dispatcher_register(g_dispatcher_plugin_d, "list", on_list_method, NULL);
    /* L2 协议标准方法 + 标准名别名（02-l2-service-protocol.md：plugin.install / plugin.uninstall / plugin.health_check） */
    method_dispatcher_register(g_dispatcher_plugin_d, "install", on_load_method, NULL);
    method_dispatcher_register(g_dispatcher_plugin_d, "uninstall", on_unload_method, NULL);
    method_dispatcher_register(g_dispatcher_plugin_d, "health_check", on_health_check_method, NULL);
    /* L2 协议标准方法 <ns>.shutdown（02-l2-service-protocol.md §6.1：优雅停止） */
    method_dispatcher_register(g_dispatcher_plugin_d, "shutdown", on_shutdown_method_plugin_d, NULL);
    SVC_LOG_INFO("Plugin_d: registered 13 RPC methods (plugin.* namespace)");

    if (daemon_event_driver_add_server_fd(g_event_driver_plugin_d,
                                          (int)server_fd) != 0) {
        SVC_LOG_ERROR("Plugin_d: failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_plugin_d);
        airy_sock_close(server_fd);
        destroy_service();
        airy_mtx_destroy(&g_running_lock_plugin_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("Plugin_d: running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_plugin_d);

    daemon_cleanup_standard(g_bipc_plugin_d, g_bsd_plugin_d,
                             g_event_driver_plugin_d, server_fd,
                             destroy_service, &g_running_lock_plugin_d);

    SVC_LOG_INFO("Plugin_d: stopped");
    log_cleanup();
    return 0;
}
