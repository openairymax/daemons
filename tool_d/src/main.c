// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file main.c
 * @brief Tool service daemon main entry (daemon module conventions).
 */

#include "daemon_main.h"
#include "platform.h"
#include "param_validator.h"
#include "svc_logger.h"
#include "thread_pool.h"
#include "tool_service.h"

#include <stdlib.h>
#include <time.h>

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("tool.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_tool"
#define DEFAULT_TCP_PORT 8081
#define MAX_BUFFER 65536
#define MAX_CLIENTS 64

DAEMON_DECLARE_COMMON(tool_d, tool, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(tool_d)

static tool_service_t *g_service = NULL;

typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_clients;
} tool_daemon_config_t;

static tool_daemon_config_t g_config = {0};

#ifdef _WIN32

static BOOL WINAPI console_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_tool_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static void handle_register(cJSON *params, int id, airy_sock_t fd);
static void handle_list(int id, airy_sock_t fd);
static void handle_get(cJSON *params, int id, airy_sock_t fd);
static void handle_execute(cJSON *params, int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);

static void handle_pending(int id, airy_sock_t fd);
static void handle_approve(cJSON *params, int id, airy_sock_t fd);

static void on_register_method(cJSON *params, int id, void *user_data)
{
    handle_register(params, id, *(airy_sock_t *)user_data);
}

static void on_list_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_list(id, *(airy_sock_t *)user_data);
}

static void on_get_method(cJSON *params, int id, void *user_data)
{
    handle_get(params, id, *(airy_sock_t *)user_data);
}

static void on_execute_method(cJSON *params, int id, void *user_data)
{
    handle_execute(params, id, *(airy_sock_t *)user_data);
}

static void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
}

static void on_pending_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_pending(id, *(airy_sock_t *)user_data);
}

static void on_approve_method(cJSON *params, int id, void *user_data)
{
    handle_approve(params, id, *(airy_sock_t *)user_data);
}

static void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Tool service not ready", id);
        return;
    }
    char *stats_json = tool_service_get_stats(g_service);
    if (stats_json) {

        cJSON *result = cJSON_Parse(stats_json);
        AIRY_FREE(stats_json);
        if (result) {
            JSONRPC_SEND_SUCCESS(client_fd, result, id);
        } else {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Stats serialization failed", id);
        }
    } else {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Failed to collect stats", id);
    }
}

static void handle_register(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *tool = jsonrpc_get_object_param(params, "tool");
    if (!tool) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing tool object", id);
        return;
    }

    tool_metadata_t meta = {0};
    const char *tid = get_string_field(tool, "id", NULL);
    const char *tname = get_string_field(tool, "name", NULL);
    const char *texec = get_string_field(tool, "executable", NULL);

    if (!tid || !tname || !texec) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Invalid tool fields: id, name, executable required", id);
        return;
    }

    meta.id = (char *)tid;
    meta.name = (char *)tname;
    meta.executable = (char *)texec;

    meta.description = (char *)get_string_field(tool, "description", NULL);
    meta.timeout_sec = get_int_field(tool, "timeout_sec", 0);
    meta.cacheable = get_bool_field(tool, "cacheable", false);
    meta.permission_rule = (char *)get_string_field(tool, "permission_rule", NULL);

    cJSON *params_arr = cJSON_GetObjectItem(tool, "params");
    if (cJSON_IsArray(params_arr)) {
        size_t cnt = cJSON_GetArraySize(params_arr);
        tool_param_t *p = (tool_param_t *)AIRY_CALLOC(cnt, sizeof(tool_param_t));
        if (p) {
            for (size_t i = 0; i < cnt; ++i) {
                cJSON *item = cJSON_GetArrayItem(params_arr, i);
                cJSON *pname = cJSON_GetObjectItem(item, "name");
                cJSON *pschema = cJSON_GetObjectItem(item, "schema");
                if (cJSON_IsString(pname))
                    p[i].name = pname->valuestring;
                if (cJSON_IsString(pschema))
                    p[i].schema = pschema->valuestring;
            }
            meta.params = p;
            meta.param_count = cnt;
        }
    }

    int ret = tool_service_register(g_service, &meta);
    AIRY_FREE((void *)meta.params);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Register failed", id);
        SVC_LOG_ERROR("Failed to register tool: %s (error=%d)", meta.id, ret);
    } else {
        JSONRPC_SEND_SUCCESS(client_fd, NULL, id);
        SVC_LOG_INFO("Tool registered successfully: %s", meta.id);
    }
}

static void handle_list(int id, airy_sock_t client_fd)
{
    char *list_json = tool_service_list(g_service);
    if (!list_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "List failed", id);
        return;
    }

    CJSON_PARSE_GUARD(result, list_json, {
        AIRY_FREE(list_json);
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Invalid JSON from list", id);
        return;
    });
    AIRY_FREE(list_json);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    result = NULL;
}

static void handle_get(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *tid = get_string_field(params, "tool_id", NULL);
    if (!tid) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing tool_id", id);
        return;
    }

    tool_metadata_t *meta = tool_service_get(g_service, tid);
    if (!meta) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Tool not found", id);
        return;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", meta->id);
    cJSON_AddStringToObject(obj, "name", meta->name);
    cJSON_AddStringToObject(obj, "executable", meta->executable);
    if (meta->description)
        cJSON_AddStringToObject(obj, "description", meta->description);
    cJSON_AddNumberToObject(obj, "timeout_sec", meta->timeout_sec);
    cJSON_AddBoolToObject(obj, "cacheable", meta->cacheable);
    if (meta->permission_rule)
        cJSON_AddStringToObject(obj, "permission_rule", meta->permission_rule);

    if (meta->param_count > 0) {
        cJSON *params_arr = cJSON_CreateArray();
        for (size_t i = 0; i < meta->param_count; ++i) {
            cJSON *pobj = cJSON_CreateObject();
            cJSON_AddStringToObject(pobj, "name", meta->params[i].name);
            cJSON_AddStringToObject(pobj, "schema", meta->params[i].schema);
            cJSON_AddItemToArray(params_arr, pobj);
        }
        cJSON_AddItemToObject(obj, "params", params_arr);
    }

    JSONRPC_SEND_SUCCESS(client_fd, obj, id);
    tool_metadata_free(meta);
}

static void handle_execute(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *tid = get_string_field(params, "tool_id", NULL);
    cJSON *jparams = jsonrpc_get_object_param(params, "params");
    /* P0 interactive approval: optionally pass through the caller's agent_id
     * (e.g. an agent child process's coding_v1) so the ACL judges by the real
     * subject; when absent, fall back to the "tool_d" default (existing
     * behavior unchanged). */
    const char *agent_id = get_string_field(params, "agent_id", NULL);

    if (!tid || !jparams) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Invalid execute params: tool_id and params required", id);
        return;
    }

    char *params_json = cJSON_PrintUnformatted(jparams);
    if (!params_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "JSON serialization failed", id);
        return;
    }

    tool_execute_request_t req = {.tool_id = tid,
                                  .params_json = params_json,
                                  .stream = 0,
                                  .agent_id = agent_id};

    tool_result_t *res = NULL;
    int ret = tool_service_execute(g_service, &req, &res);
    AIRY_FREE((void *)params_json);

    if (ret != AIRY_SUCCESS || !res) {
        /* Prefer passing through the executor's error description (e.g. the
         * "User denied tool execution" of interactive approval deny/timeout),
         * otherwise fall back to generic info. */
        const char *emsg = (res && res->error) ? res->error : "Execution failed";
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, emsg, id);
        if (res) {
            tool_result_free(res);
        }
        SVC_LOG_ERROR("Tool execution failed: %s (error=%d)", tid, ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "success", res->success);
    if (res->output)
        cJSON_AddStringToObject(result, "output", res->output);
    if (res->error)
        cJSON_AddStringToObject(result, "error", res->error);
    cJSON_AddNumberToObject(result, "exit_code", res->exit_code);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    tool_result_free(res);
}

static void handle_health_check(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "tool_d");
    cJSON_AddBoolToObject(result, "healthy", g_service != NULL);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_pending(int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Tool service not ready", id);
        return;
    }
    char *pending_json = tool_service_interactive_pending_list(g_service);
    cJSON *arr = pending_json ? cJSON_Parse(pending_json) : NULL;
    AIRY_FREE(pending_json);
    if (!arr) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Failed to list pending approvals",
                           id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "pending", arr);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_approve(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Tool service not ready", id);
        return;
    }
    const char *request_id = get_string_field(params, "request_id", NULL);
    const char *decision = get_string_field(params, "decision", NULL);
    if (!request_id || !decision) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "request_id and decision required",
                           id);
        return;
    }
    int ret = tool_service_interactive_resolve(g_service, request_id, decision);
    if (ret == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "resolved", true);
        cJSON_AddStringToObject(result, "request_id", request_id);
        cJSON_AddStringToObject(result, "decision", decision);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_INFO("P0: tool.approve resolved request_id=%s decision=%s", request_id, decision);
    } else if (ret == AIRY_ERR_NOT_FOUND) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Approval request not found", id);
    } else if (ret == AIRY_ERR_INVALID_PARAM) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Invalid decision (allow/always/deny)", id);
    } else {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Failed to resolve approval", id);
    }
}

static int load_daemon_config(const char *config_path)
{

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

static void destroy_service(void)
{
    if (g_service) {
        tool_service_destroy(g_service);
        g_service = NULL;
    }
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_tool_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_tool_d);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    DAEMON_SETUP_SIGNALS(tool_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    daemon_cupolas_init("tool_d");

    load_daemon_config(config_path);
    if (use_tcp)
        g_config.use_tcp = 1;

    SVC_LOG_INFO("Tool service starting, manager=%s", config_path ? config_path : "default");

    g_service =
        tool_service_create(config_path ? config_path : "agentrt/manager/service/tool_d/tool.yaml");
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create tool service");
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_tool_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    airy_sock_t server_fd = daemon_create_server_socket(g_config.use_tcp, g_config.tcp_port,
                                                        g_config.socket_path, g_config.socket_path);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_tool_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO(g_config.use_tcp ? "Listening on TCP %s:%d" : "Listening on %s", g_config.tcp_host,
                 g_config.tcp_port);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = 4;
    ev_config.thread_pool_max = 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_tool_d;
    ev_config.service_ctx = NULL;
    /* P0 interactive approval: client requests are dispatched to the thread
     * pool for concurrent handling. Otherwise, execute blocking on a
     * tool.approve decision would stall the event-loop thread and pending/
     * approve requests could never be processed. */
    ev_config.concurrent_clients = true;

    const char *sock_addr = g_config.use_tcp ? g_config.tcp_host : g_config.socket_path;
    int ret = daemon_init_event_driver("tool_d", "tool", sock_addr,
                                       g_config.use_tcp ? g_config.tcp_port : 0, "tool,core",
                                       g_config.use_tcp, &ev_config, &g_event_driver_tool_d,
                                       &g_bsd_tool_d, &g_bipc_tool_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_tool_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_tool_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_tool_d = daemon_event_driver_get_dispatcher(g_event_driver_tool_d);
    method_dispatcher_register(g_dispatcher_tool_d, "register", on_register_method, NULL);
    method_dispatcher_register(g_dispatcher_tool_d, "list_tools", on_list_method, NULL);
    method_dispatcher_register(g_dispatcher_tool_d, "get_tool", on_get_method, NULL);
    method_dispatcher_register(g_dispatcher_tool_d, "execute_tool", on_execute_method, NULL);
    /* Standard L2 protocol methods + standard-name aliases
     * (02-l2-service-protocol.md: tool.execute / tool.list / tool.health_check)
     */
    method_dispatcher_register(g_dispatcher_tool_d, "execute", on_execute_method, NULL);
    method_dispatcher_register(g_dispatcher_tool_d, "list", on_list_method, NULL);
    method_dispatcher_register(g_dispatcher_tool_d, "health_check", on_health_check_method, NULL);

    method_dispatcher_register(g_dispatcher_tool_d, "shutdown", on_shutdown_method_tool_d, NULL);

    method_dispatcher_register(g_dispatcher_tool_d, "get_stats", on_get_stats_method, NULL);

    method_dispatcher_register(g_dispatcher_tool_d, "pending", on_pending_method, NULL);
    method_dispatcher_register(g_dispatcher_tool_d, "approve", on_approve_method, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (tool.* namespace)", 11);

    if (daemon_event_driver_add_server_fd(g_event_driver_tool_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_tool_d);
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_tool_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("Tool service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_tool_d);

    daemon_cleanup_standard(g_bipc_tool_d, g_bsd_tool_d, g_event_driver_tool_d, server_fd,
                            g_config.socket_path, destroy_service, &g_running_lock_tool_d);
    free_daemon_config();

    SVC_LOG_INFO("Tool service stopped");
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
