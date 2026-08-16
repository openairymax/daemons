// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file main.c
 * @brief Hook daemon entry (P0.18.1 boilerplate macros).
 * @owner team-A
 *
 * Exposes JSON-RPC methods (hook.* namespace):
 *   - hook.health : registry health status + total registered Hook count
 *   - hook.ping   : liveness probe (with uptime)
 *   - hook.status : real status (total / per-type counts)
 *   - hook.list   : list registered (enabled) Hooks with their stats
 *   - hook.stats  : query a single Hook's stats by name
 *
 * Data source: the hook_registry in atoms/coreloopthree/src/hook/ (linked
 * via airy_coreloopthree).
 * Unix socket path: ${AIRY_RUNTIME_DIR}/hook.sock
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
#define HOOK_D_PIPE_PATH "\\\\.\\pipe\\airy_hook"
#define HOOK_D_DEFAULT_PORT 8093
#define HOOK_D_MAX_BUFFER 4096

DAEMON_DECLARE_COMMON(hook_d, hook, HOOK_D_SOCKET_PATH, HOOK_D_PIPE_PATH, HOOK_D_DEFAULT_PORT,
                      HOOK_D_MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(hook_d)

static int g_registry_initialized = 0;
static uint64_t g_start_time = 0;

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
    case HOOK_TYPE_PRE_EXEC:
        return "pre_exec";
    case HOOK_TYPE_POST_EXEC:
        return "post_exec";
    case HOOK_TYPE_PRE_LLM:
        return "pre_llm";
    case HOOK_TYPE_POST_LLM:
        return "post_llm";
    case HOOK_TYPE_PRE_TOOL:
        return "pre_tool";
    case HOOK_TYPE_POST_TOOL:
        return "post_tool";
    case HOOK_TYPE_ON_ERROR:
        return "on_error";
    case HOOK_TYPE_ON_MEMORY_EVOLVE:
        return "on_memory_evolve";
    default:
        return "unknown";
    }
}

static int hook_type_from_name(const char *name)
{
    if (!name)
        return -1;
    for (int t = 0; t < HOOK_TYPE_COUNT; t++) {
        if (strcmp(name, hook_type_name((hook_type_t)t)) == 0)
            return t;
    }
    return -1;
}

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

static void hook_on_get_stats(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "daemon", "hook_d");
    cJSON_AddNumberToObject(result, "hooks", (double)hook_registry_count());
    if (g_start_time > 0) {
        cJSON_AddNumberToObject(result, "uptime_s", (double)((uint64_t)time(NULL) - g_start_time));
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ==================== L2 standard methods (02-l2-service-protocol.md:
 * hook.register / hook.unregister / hook.trigger / hook.health_check)
 * ====================
 */

/*
 * hook.register: register script-type Hooks into the hook_registry (RPC
 * cannot pass C callbacks, so shell/python/webhook implementation types are
 * supported; the CALLBACK type is limited to built-in handlers).
 * params: name(required), type(string or int, e.g. "pre_exec"),
 * impl("shell"/"python"/"webhook"), script_path(script path or Webhook URL),
 * priority(default 0), enabled(default true)
 */
static void hook_on_register(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;

    const char *name = jsonrpc_get_string_param(params, "name", NULL);
    if (!name || !name[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing hook name", id);
        return;
    }

    int type = -1;
    cJSON *type_json = cJSON_GetObjectItem(params, "type");
    if (cJSON_IsNumber(type_json)) {
        type = type_json->valueint;
    } else if (cJSON_IsString(type_json)) {
        type = hook_type_from_name(type_json->valuestring);
    }
    if (type < 0 || type >= HOOK_TYPE_COUNT) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Invalid hook type", id);
        return;
    }

    const char *impl_str = jsonrpc_get_string_param(params, "impl", "shell");
    hook_impl_type_t impl_type = HOOK_IMPL_SHELL;
    if (strcmp(impl_str, "python") == 0)
        impl_type = HOOK_IMPL_PYTHON;
    else if (strcmp(impl_str, "webhook") == 0)
        impl_type = HOOK_IMPL_WEBHOOK;
    else if (strcmp(impl_str, "callback") == 0)
        impl_type = HOOK_IMPL_CALLBACK;

    const char *script_path = jsonrpc_get_string_param(params, "script_path", NULL);
    if (impl_type != HOOK_IMPL_CALLBACK && (!script_path || !script_path[0])) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing script_path for script/webhook hook", id);
        return;
    }

    int priority = 0;
    cJSON *priority_json = cJSON_GetObjectItem(params, "priority");
    if (cJSON_IsNumber(priority_json))
        priority = priority_json->valueint;
    cJSON *enabled_json = cJSON_GetObjectItem(params, "enabled");
    bool enabled = !cJSON_IsFalse(enabled_json);

    hook_entry_t entry;
    __builtin_memset(&entry, 0, sizeof(entry));
    AIRY_STRNCPY_TERM(entry.name, name, sizeof(entry.name));
    entry.type = (hook_type_t)type;
    entry.impl_type = impl_type;
    if (script_path)
        AIRY_STRNCPY_TERM(entry.script_path, script_path, sizeof(entry.script_path));
    entry.priority = priority;
    entry.enabled = enabled;

    int ret = hook_registry_register(&entry);
    if (ret != 0) {
        const char *msg = ret == -3 ? "Hook name already registered" :
                          ret == -2 ? "Hook registry full" :
                                      "Hook register failed";
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, msg, id);
        SVC_LOG_ERROR("hook.register failed: name=%s error=%d", name, ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "registered");
    cJSON_AddStringToObject(result, "name", name);
    cJSON_AddStringToObject(result, "type", hook_type_name((hook_type_t)type));
    cJSON_AddBoolToObject(result, "enabled", enabled);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("hook.register OK: name=%s type=%s impl=%s", name,
                 hook_type_name((hook_type_t)type), impl_str);
}

static void hook_on_unregister(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    const char *name = jsonrpc_get_string_param(params, "name", NULL);
    if (!name || !name[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing hook name", id);
        return;
    }

    int ret = hook_registry_unregister(name);
    if (ret != 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Hook not found", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "unregistered");
    cJSON_AddStringToObject(result, "name", name);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("hook.unregister OK: name=%s", name);
}

/* hook.trigger: trigger the hook chain by type (hook_service_fire aggregates
 * the decision)
 * params: type(string or int), operation(optional), input(optional text) */
static void hook_on_trigger(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;

    int type = -1;
    cJSON *type_json = cJSON_GetObjectItem(params, "type");
    if (cJSON_IsNumber(type_json)) {
        type = type_json->valueint;
    } else if (cJSON_IsString(type_json)) {
        type = hook_type_from_name(type_json->valuestring);
    }
    if (type < 0 || type >= HOOK_TYPE_COUNT) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Invalid hook type", id);
        return;
    }

    const char *operation = jsonrpc_get_string_param(params, "operation", NULL);
    const char *input = jsonrpc_get_string_param(params, "input", NULL);
    const char *hook_name = jsonrpc_get_string_param(params, "hook_name", NULL);

    hook_context_t ctx;
    __builtin_memset(&ctx, 0, sizeof(ctx));
    ctx.type = (hook_type_t)type;
    ctx.hook_name = hook_name;
    ctx.operation = operation;
    ctx.input_data = input;
    ctx.input_data_len = input ? strlen(input) : 0;
    ctx.timestamp_ns = (uint64_t)time(NULL) * 1000000000ull;

    hook_decision_t decision = hook_service_fire(&ctx);

    const char *decision_name = "continue";
    switch (decision) {
    case HOOK_DECISION_SKIP:
        decision_name = "skip";
        break;
    case HOOK_DECISION_RETRY:
        decision_name = "retry";
        break;
    case HOOK_DECISION_ABORT:
        decision_name = "abort";
        break;
    case HOOK_DECISION_MODIFY:
        decision_name = "modify";
        break;
    default:
        break;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "decision", (double)decision);
    cJSON_AddStringToObject(result, "decision_name", decision_name);
    cJSON_AddStringToObject(result, "type", hook_type_name((hook_type_t)type));
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("hook.trigger OK: type=%s decision=%s", hook_type_name((hook_type_t)type),
                 decision_name);
}

static void hook_on_health_check(cJSON *params, int id, void *user_data)
{
    (void)params;
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "hook_d");
    cJSON_AddBoolToObject(result, "healthy", g_registry_initialized ? true : false);
    cJSON_AddNumberToObject(result, "hook_count", (double)hook_registry_count());
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

int main(int argc, char *argv[])
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_hook_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;
    (void)config_path;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_hook_d);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler_hook_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(hook_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    daemon_cupolas_init("hook_d");
    g_start_time = (uint64_t)time(NULL);
    SVC_LOG_INFO("hook_d: starting");

    if (hook_registry_init() == 0) {
        g_registry_initialized = 1;
        SVC_LOG_INFO("hook_d: hook registry initialized");
        /* Register the built-in production hook handlers (audit/metrics/
         * trace, 12 in total); status/list thus returns real loaded hook
         * module info */
        airy_hook_register_builtin_handlers();
    } else {
        SVC_LOG_ERROR("hook_d: hook registry init failed");
    }

    airy_sock_t server_fd =
        daemon_create_server_socket(use_tcp, HOOK_D_DEFAULT_PORT, HOOK_D_SOCKET_PATH, HOOK_D_PIPE_PATH);
    if (server_fd < 0) {
        SVC_LOG_ERROR("hook_d: failed to create socket at %s (errno=%d: %s)", HOOK_D_SOCKET_PATH,
                      errno, strerror(errno));
        airy_mtx_destroy(&g_running_lock_hook_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO("hook_d: listening on %s (fd=%d)", HOOK_D_SOCKET_PATH, (int)server_fd);

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
    int ret = daemon_init_event_driver("hook_d", "hook", sock_addr, use_tcp ? HOOK_D_DEFAULT_PORT : 0,
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
    /* Standard L2 protocol methods (02-l2-service-protocol.md: hook.register /
     * hook.unregister / hook.trigger / hook.health_check)
     */
    method_dispatcher_register(g_dispatcher_hook_d, "register", hook_on_register, NULL);
    method_dispatcher_register(g_dispatcher_hook_d, "unregister", hook_on_unregister, NULL);
    method_dispatcher_register(g_dispatcher_hook_d, "trigger", hook_on_trigger, NULL);
    method_dispatcher_register(g_dispatcher_hook_d, "health_check", hook_on_health_check, NULL);

    method_dispatcher_register(g_dispatcher_hook_d, "shutdown", on_shutdown_method_hook_d, NULL);

    method_dispatcher_register(g_dispatcher_hook_d, "get_stats", hook_on_get_stats, NULL);
    SVC_LOG_INFO("hook_d: registered 11 RPC methods (hook.* namespace)");

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
