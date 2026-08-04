// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file main.c
 * @brief Hook 守护进程入口（P0.18.1 样板宏化）
 * @owner team-A
 *
 * 暴露 JSON-RPC 方法（hook.* 命名空间）：
 *   - hook.health : 注册表健康状态 + 已注册 Hook 总数
 *   - hook.ping   : 存活探针（含 uptime）
 *   - hook.status : 真实状态（总数 / 各类型计数）
 *   - hook.list   : 列出已注册（启用）的 Hook 及其统计
 *   - hook.stats  : 按名称查询单个 Hook 统计
 *
 * 数据源：atoms/coreloopthree/src/hook/ 的 hook_registry（经 airy_coreloopthree 链接）。
 * Unix socket 路径：${AIRY_RUNTIME_DIR}/hook.sock
 */

#include "airy_memory.h"
#include "error.h"
#include "daemon_main.h"
#include "platform.h"
#include "hook_service.h"
#include "hook_registry.h"
#include "hook_builtin_handlers.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HOOK_D_SOCKET_PATH airy_runtime_dir_socket("hook.sock")
#define HOOK_D_PIPE_PATH   "\\\\.\\pipe\\airy_hook"
#define HOOK_D_MAX_BUFFER  4096

/* P0.18.1: 使用 DAEMON_DECLARE_COMMON 生成公共样板（信号处理/全局变量/print_usage） */
DAEMON_DECLARE_COMMON(hook_d, hook, HOOK_D_SOCKET_PATH, HOOK_D_PIPE_PATH, 0, HOOK_D_MAX_BUFFER)

static int g_registry_initialized = 0;
static uint64_t g_start_time = 0;

/* 销毁服务（daemon_cleanup_standard 回调） */
static void destroy_service_hook_d(void)
{
    if (g_registry_initialized) {
        airy_hook_unregister_builtin_handlers();
        hook_registry_destroy();
        g_registry_initialized = 0;
    }
    daemon_cupolas_cleanup();
}

#ifdef _WIN32
/* Windows 控制台事件处理（复用生成的 signal_handler_hook_d） */
static BOOL WINAPI console_handler_hook_d(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_hook_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static const char *hook_type_name(hook_type_t type)
{
    switch (type) {
    case HOOK_TYPE_PRE_EXEC:         return "pre_exec";
    case HOOK_TYPE_POST_EXEC:        return "post_exec";
    case HOOK_TYPE_PRE_LLM:          return "pre_llm";
    case HOOK_TYPE_POST_LLM:         return "post_llm";
    case HOOK_TYPE_PRE_TOOL:         return "pre_tool";
    case HOOK_TYPE_POST_TOOL:        return "post_tool";
    case HOOK_TYPE_ON_ERROR:         return "on_error";
    case HOOK_TYPE_ON_MEMORY_EVOLVE: return "on_memory_evolve";
    default:                         return "unknown";
    }
}

/* ==================== JSON-RPC 方法 ==================== */

static void hook_on_health(cJSON *params, int id, void *user_data)
{
    (void)params;
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "healthy", g_registry_initialized ? true : false);
    cJSON_AddNumberToObject(result, "hook_count", (double)hook_registry_count());
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void hook_on_ping(cJSON *params, int id, void *user_data)
{
    (void)params;
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");
    cJSON_AddNumberToObject(result, "uptime_sec", (double)(time(NULL) - (time_t)g_start_time));
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void hook_on_status(cJSON *params, int id, void *user_data)
{
    (void)params;
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "hook_d");
    cJSON_AddNumberToObject(result, "hook_count", (double)hook_registry_count());
    cJSON_AddBoolToObject(result, "registry_initialized", g_registry_initialized ? true : false);

    cJSON *by_type = cJSON_CreateObject();
    for (int t = 0; t < HOOK_TYPE_COUNT; t++) {
        cJSON_AddNumberToObject(by_type, hook_type_name((hook_type_t)t),
                                (double)hook_registry_count_by_type((hook_type_t)t));
    }
    cJSON_AddItemToObject(result, "by_type", by_type);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void hook_on_list(cJSON *params, int id, void *user_data)
{
    (void)params;
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *result = cJSON_CreateObject();
    cJSON *hooks = cJSON_CreateArray();

    for (int t = 0; t < HOOK_TYPE_COUNT; t++) {
        hook_entry_t *entries[HOOK_REGISTRY_MAX];
        size_t count = 0;
        if (hook_registry_get_by_type((hook_type_t)t, entries, HOOK_REGISTRY_MAX, &count) != 0)
            continue;
        for (size_t i = 0; i < count; i++) {
            const hook_entry_t *e = entries[i];
            cJSON *h = cJSON_CreateObject();
            cJSON_AddStringToObject(h, "name", e->name);
            cJSON_AddStringToObject(h, "type", hook_type_name(e->type));
            cJSON_AddNumberToObject(h, "type_id", (double)e->type);
            cJSON_AddNumberToObject(h, "impl_type", (double)e->impl_type);
            cJSON_AddNumberToObject(h, "priority", (double)e->priority);
            cJSON_AddBoolToObject(h, "enabled", e->enabled);
            cJSON_AddNumberToObject(h, "invoke_count", (double)e->invoke_count);
            cJSON_AddNumberToObject(h, "skip_count", (double)e->skip_count);
            cJSON_AddNumberToObject(h, "abort_count", (double)e->abort_count);
            cJSON_AddNumberToObject(h, "total_duration_ns", (double)e->total_duration_ns);
            if (e->script_path[0])
                cJSON_AddStringToObject(h, "script_path", e->script_path);
            cJSON_AddItemToArray(hooks, h);
        }
    }
    cJSON_AddItemToObject(result, "hooks", hooks);
    cJSON_AddNumberToObject(result, "count", (double)hook_registry_count());
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void hook_on_stats(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    const char *name = jsonrpc_get_string_param(params, "name", NULL);
    if (!name || !name[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing hook name", id);
        return;
    }
    hook_stats_t stats;
    if (hook_registry_get_stats(name, &stats) != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Hook not found", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "name", name);
    cJSON_AddNumberToObject(result, "invoke_count", (double)stats.invoke_count);
    cJSON_AddNumberToObject(result, "skip_count", (double)stats.skip_count);
    cJSON_AddNumberToObject(result, "abort_count", (double)stats.abort_count);
    cJSON_AddNumberToObject(result, "retry_count", (double)stats.retry_count);
    cJSON_AddNumberToObject(result, "modify_count", (double)stats.modify_count);
    cJSON_AddNumberToObject(result, "total_duration_ns", (double)stats.total_duration_ns);
    cJSON_AddNumberToObject(result, "max_duration_ns", (double)stats.max_duration_ns);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ==================== 主入口 ==================== */

int main(int argc, char *argv[])
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_hook_d);
    if (parse_rc > 0) return parse_rc == 1 ? 0 : 1;
    (void)config_path;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_hook_d);

    /* 跨平台信号处理 */
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler_hook_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(hook_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    /* P3.14 ACC-DT15: 初始化 cupolas 安全穹顶 */
    daemon_cupolas_init("hook_d");
    g_start_time = (uint64_t)time(NULL);
    SVC_LOG_INFO("hook_d: starting");

    /* 初始化 hook 注册表（status/list 的真实数据源） */
    if (hook_registry_init() == 0) {
        g_registry_initialized = 1;
        SVC_LOG_INFO("hook_d: hook registry initialized");
        /* 注册内置生产 Hook 处理器（audit/metrics/trace，共 12 个），
           status/list 由此返回真实已加载的 hook 模块信息 */
        airy_hook_register_builtin_handlers();
    } else {
        SVC_LOG_ERROR("hook_d: hook registry init failed");
    }

    /* 创建 Socket 服务器 */
    airy_sock_t server_fd =
        daemon_create_server_socket(use_tcp, 0, HOOK_D_SOCKET_PATH, HOOK_D_PIPE_PATH);
    if (server_fd < 0) {
        SVC_LOG_ERROR("hook_d: failed to create socket at %s (errno=%d: %s)",
                      HOOK_D_SOCKET_PATH, errno, strerror(errno));
        airy_mtx_destroy(&g_running_lock_hook_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO("hook_d: listening on %s (fd=%d)", HOOK_D_SOCKET_PATH, (int)server_fd);

    /* 事件驱动（mem_d 同款模式）：统一 accept + JSON-RPC 分发 */
    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = 2;
    ev_config.thread_pool_max = 4;
    ev_config.thread_pool_queue_size = 128;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_hook_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = use_tcp ? "127.0.0.1" : HOOK_D_SOCKET_PATH;
    int ret = daemon_init_event_driver("hook_d", "hook", sock_addr, use_tcp ? 0 : 0,
                                       "hook,core", use_tcp, &ev_config, &g_event_driver_hook_d,
                                       &g_bsd_hook_d, &g_bipc_hook_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_hook_d) {
        SVC_LOG_ERROR("hook_d: failed to create event driver");
        airy_sock_close(server_fd);
        airy_mtx_destroy(&g_running_lock_hook_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_hook_d = daemon_event_driver_get_dispatcher(g_event_driver_hook_d);
    method_dispatcher_register(g_dispatcher_hook_d, "health", hook_on_health, NULL);
    method_dispatcher_register(g_dispatcher_hook_d, "ping", hook_on_ping, NULL);
    method_dispatcher_register(g_dispatcher_hook_d, "status", hook_on_status, NULL);
    method_dispatcher_register(g_dispatcher_hook_d, "list", hook_on_list, NULL);
    method_dispatcher_register(g_dispatcher_hook_d, "stats", hook_on_stats, NULL);
    SVC_LOG_INFO("hook_d: registered 5 RPC methods (hook.* namespace)");

    if (daemon_event_driver_add_server_fd(g_event_driver_hook_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("hook_d: failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_hook_d);
        airy_sock_close(server_fd);
        airy_mtx_destroy(&g_running_lock_hook_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("hook_d: running (event-driven mode), waiting for requests");
    daemon_event_driver_run(g_event_driver_hook_d);

    SVC_LOG_INFO("hook_d: shutting down");
    daemon_cleanup_standard(g_bipc_hook_d, g_bsd_hook_d, g_event_driver_hook_d, server_fd,
                            destroy_service_hook_d, &g_running_lock_hook_d);
    log_cleanup();
    return EXIT_SUCCESS;
}
