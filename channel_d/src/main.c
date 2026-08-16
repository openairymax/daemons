// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file main.c
 * @brief Channel daemon main entry (P0.18.1 boilerplate macros).
 */

#include "channel_service.h"
#include "daemon_main.h"
#include "platform.h"

#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>

#define CHANNEL_D_SOCKET_PATH airy_runtime_dir_socket("channel.sock")
#define CHANNEL_D_PIPE_PATH "\\\\.\\pipe\\airy_channel"
#define CHANNEL_D_DEFAULT_PORT 8094

DAEMON_DECLARE_COMMON(channel_d, channel, CHANNEL_D_SOCKET_PATH, CHANNEL_D_PIPE_PATH,
                      CHANNEL_D_DEFAULT_PORT, 65536)

DAEMON_DECLARE_SHUTDOWN_METHOD(channel_d)

static channel_service_t *g_svc __attribute__((unused)) = NULL;

static void destroy_service_channel_d(void)
{
    if (g_svc) {
        channel_service_stop(g_svc);
        channel_service_destroy(g_svc);
        g_svc = NULL;
    }
    daemon_cupolas_cleanup();
}

#ifdef _WIN32
static BOOL WINAPI console_handler_channel_d(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_channel_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

/* Extract channel_id from cJSON params: supports string and numeric ids,
 * returning an AIRY_MALLOC string or NULL.
 * (Fix: the original hand-written strstr/strchr parsing could not handle
 * numeric ids nor data containing escaped quotes, silently dropping send data
 * while still reporting success — switched to cJSON parsing, failing closed
 * with an error.) */
static char *channel_param_id_str(cJSON *params)
{
    cJSON *id = cJSON_GetObjectItem(params, "id");
    if (cJSON_IsString(id) && id->valuestring && id->valuestring[0])
        return AIRY_STRDUP(id->valuestring);
    if (cJSON_IsNumber(id)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", id->valueint);
        return AIRY_STRDUP(buf);
    }
    return NULL;
}

__attribute__((used)) static int handle_service_request(const char *method, cJSON *params,
                                                        char **response_json, void *user_data)
{
    channel_service_t *svc = (channel_service_t *)user_data;
    if (!svc || !method || !response_json) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    if (!params) {
        params = cJSON_CreateObject();
        if (!params)
            AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to create empty params");
    }

    if (strcmp(method, "ping") == 0) {
        char *id = channel_param_id_str(params);
        if (!id) {
            bool healthy = channel_service_is_healthy(svc);
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"status\":\"%s\"}", healthy ? "ok" : "degraded");
            *response_json = AIRY_STRDUP(buf);
            return 0;
        }
        int64_t latency_ms = 0;
        int rc = channel_service_ping(svc, id, &latency_ms);
        if (rc != CHANNEL_OK) {
            AIRY_FREE(id);
            char err[256];
            snprintf(err, sizeof(err), "{\"error\":\"ping failed: %d\",\"latency_ms\":%lld}", rc,
                     (long long)latency_ms);
            *response_json = AIRY_STRDUP(err);
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_ping failed");
        }
        size_t sz =
            snprintf(NULL, 0, "{\"status\":\"ok\",\"channel_id\":\"%s\",\"latency_ms\":%lld}", id,
                     (long long)latency_ms) +
            1;
        char *buf = (char *)AIRY_MALLOC(sz);
        if (!buf) {
            AIRY_FREE(id);
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "malloc failed for ping response buffer");
        }
        snprintf(buf, sz, "{\"status\":\"ok\",\"channel_id\":\"%s\",\"latency_ms\":%lld}", id,
                 (long long)latency_ms);
        AIRY_FREE(id);
        *response_json = buf;
        return 0;
    }

    if (strcmp(method, "list") == 0) {
        channel_info_t info_list[CHANNEL_MAX_CHANNELS];
        size_t count = 0;
        int rc = channel_service_list(svc, info_list, CHANNEL_MAX_CHANNELS, &count);
        if (rc != 0) {
            *response_json = AIRY_STRDUP("{\"error\":\"list failed\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_list failed");
        }

        size_t buf_size = 4096 + count * 1024;
        char *buf = (char *)AIRY_MALLOC(buf_size);
        if (!buf) {
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "malloc failed for list response buffer");
        }

        size_t pos = 0;
        pos += snprintf(buf + pos, buf_size - pos, "{\"channels\":[");
        for (size_t i = 0; i < count; i++) {
            if (i > 0)
                pos += snprintf(buf + pos, buf_size - pos, ",");
            pos += snprintf(buf + pos, buf_size - pos,
                            "{\"id\":\"%s\",\"name\":\"%s\",\"type\":%d,\"status\":%d,\"sent\":%zu,"
                            "\"recv\":%zu}",
                            info_list[i].channel_id, info_list[i].name, info_list[i].type,
                            info_list[i].status, info_list[i].messages_sent,
                            info_list[i].messages_received);
        }
        pos += snprintf(buf + pos, buf_size - pos, "]}");
        *response_json = buf;
        return 0;
    }

    if (strcmp(method, "stats") == 0) {
        /* L2 standard method channel.get_stats (02-l2-service-protocol.md
         * §6.1): returns the channel count + cumulative sent/received message
         * counts per channel (real statistics). */
        channel_info_t info_list[CHANNEL_MAX_CHANNELS];
        size_t count = 0;
        int rc = channel_service_list(svc, info_list, CHANNEL_MAX_CHANNELS, &count);
        if (rc != 0) {
            *response_json = AIRY_STRDUP("{\"error\":\"list failed\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_list failed in stats");
        }
        uint64_t sent = 0, recv = 0;
        for (size_t i = 0; i < count; i++) {
            sent += info_list[i].messages_sent;
            recv += info_list[i].messages_received;
        }
        size_t sz = snprintf(NULL, 0,
                             "{\"daemon\":\"channel_d\",\"channels\":%zu,"
                             "\"messages_sent\":%llu,\"messages_received\":%llu}",
                             count, (unsigned long long)sent, (unsigned long long)recv) +
                    1;
        char *buf = (char *)AIRY_MALLOC(sz);
        if (!buf) {
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "malloc failed for stats response buffer");
        }
        snprintf(buf, sz,
                 "{\"daemon\":\"channel_d\",\"channels\":%zu,"
                 "\"messages_sent\":%llu,\"messages_received\":%llu}",
                 count, (unsigned long long)sent, (unsigned long long)recv);
        *response_json = buf;
        return 0;
    }

    if (strcmp(method, "open") == 0) {
        cJSON *id = cJSON_GetObjectItem(params, "id");
        cJSON *name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(id) || !id->valuestring[0] || !cJSON_IsString(name) ||
            !name->valuestring[0]) {
            *response_json = AIRY_STRDUP("{\"error\":\"missing id or name\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "missing id or name in open request");
        }

        channel_type_t type = CHANNEL_TYPE_SOCKET;
        cJSON *typej = cJSON_GetObjectItem(params, "type");
        if (cJSON_IsNumber(typej) && typej->valueint >= 0 && typej->valueint <= 2)
            type = (channel_type_t)typej->valueint;

        int rc = channel_service_open(svc, id->valuestring, name->valuestring, type, NULL);
        if (rc != 0) {
            char err[256];
            snprintf(err, sizeof(err), "{\"error\":\"open failed: %d\"}", rc);
            *response_json = AIRY_STRDUP(err);
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_open failed");
        }

        *response_json = AIRY_STRDUP("{\"status\":\"opened\"}");
        return 0;
    }

    if (strcmp(method, "close") == 0) {
        cJSON *id = cJSON_GetObjectItem(params, "id");
        if (!cJSON_IsString(id) || !id->valuestring[0]) {
            *response_json = AIRY_STRDUP("{\"error\":\"missing id\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "missing id in close request");
        }

        int rc = channel_service_close(svc, id->valuestring);
        if (rc != 0) {
            *response_json = AIRY_STRDUP("{\"error\":\"close failed\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_close failed");
        }
        *response_json = AIRY_STRDUP("{\"status\":\"closed\"}");
        return 0;
    }

    if (strcmp(method, "send") == 0) {
        cJSON *id = cJSON_GetObjectItem(params, "id");
        cJSON *data = cJSON_GetObjectItem(params, "data");
        /* fail-closed: data must be a string; non-string (number/object/array)
         * or missing returns an error — silent skipping that loses messages
         * while reporting success is forbidden */
        if (!cJSON_IsString(id) || !id->valuestring[0] || !cJSON_IsString(data)) {
            *response_json = AIRY_STRDUP("{\"error\":\"missing id or data\"}");
            AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "missing id or data in send request");
        }
        size_t dlen = strlen(data->valuestring);
        int rc = channel_service_send(svc, id->valuestring, data->valuestring, dlen);
        if (rc != 0) {
            char err[256];
            snprintf(err, sizeof(err), "{\"error\":\"send failed: %d\"}", rc);
            *response_json = AIRY_STRDUP(err);
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_send failed");
        }
        *response_json = AIRY_STRDUP("{\"status\":\"sent\"}");
        return 0;
    }

    if (strcmp(method, "health") == 0 || strcmp(method, "health_check") == 0) {
        bool healthy = channel_service_is_healthy(svc);
        *response_json = AIRY_STRDUP(healthy ? "{\"healthy\":true}" : "{\"healthy\":false}");
        return 0;
    }

    *response_json = AIRY_STRDUP("{\"error\":\"unknown method\"}");
    AIRY_ERROR(AIRY_ERR_UNKNOWN, "unknown method");
}

/**
 * @brief Hand cJSON params directly to handle_service_request (cJSON parsing,
 *        no string round-trip) and wrap the returned JSON result as a
 *        JSON-RPC success response.
 *
 * user_data comes from daemon_handle_client_*, pointing to client_fd.
 */
static void channel_dispatch_method(cJSON *params, int id, void *user_data, const char *method)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;

    char *result_json = NULL;
    int rc = handle_service_request(method, params, &result_json, g_svc);
    if (rc != 0 || !result_json) {
        /* Param-validation failures (fail-closed: missing id/data, invalid
         * type) map to JSON-RPC Invalid params(-32602); other service errors
         * map to Internal error(-32603) */
        int code = (rc == AIRY_ERR_INVALID_PARAM) ? JSONRPC_INVALID_PARAMS : JSONRPC_INTERNAL_ERROR;
        JSONRPC_SEND_ERROR(client_fd, code, "channel service error", id);
        AIRY_FREE(result_json);
        return;
    }

    cJSON *result = cJSON_Parse(result_json);
    AIRY_FREE(result_json);
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "invalid channel response", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void channel_on_ping(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "ping");
}

static void channel_on_list(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "list");
}

static void channel_on_open(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "open");
}

static void channel_on_close(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "close");
}

static void channel_on_send(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "send");
}

static void channel_on_health(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "health");
}

static void channel_on_get_stats(cJSON *params, int id, void *user_data)
{
    channel_dispatch_method(params, id, user_data, "stats");
}

int main(int argc, char *argv[])
{
    const char *socket_dir = NULL;
    uint32_t max_channels = CHANNEL_MAX_CHANNELS;
    int use_tcp = 0;
#if defined(AIRY_PLATFORM_WINDOWS)
    /* Windows：IPC 统一走 TCP 回环（事件循环仅支持 socket）。 */
    use_tcp = 1;
#endif

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--manager") == 0 || strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            socket_dir = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_channels = (uint32_t)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--tcp") == 0) {
            use_tcp = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fputs("Usage: channel_d [--manager config] [-s socket_dir] [-n max_channels] [--tcp] [-h]\n",
                  stdout);
            return 0;
        } else {
            SVC_LOG_ERROR("Unknown option: %s", argv[i]);
            fputs("Usage: channel_d [--manager config] [-s socket_dir] [-n max_channels] [--tcp] [-h]\n",
                  stderr);
            return 1;
        }
    }

    airy_sock_init();
    airy_mtx_init(&g_running_lock_channel_d);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler_channel_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(channel_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    daemon_cupolas_init("channel_d");

    channel_config_t config = CHANNEL_CONFIG_DEFAULTS;
    config.max_channels = max_channels;
    if (socket_dir) {
        AIRY_STRNCPY_TERM(config.socket_dir, socket_dir, sizeof(config.socket_dir));
        (config.socket_dir)[sizeof(config.socket_dir) - 1] = '\0';
    }

    g_svc = channel_service_create(&config);
    if (!g_svc) {
        SVC_LOG_ERROR("Failed to create channel service");
        airy_mtx_destroy(&g_running_lock_channel_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    if (channel_service_start(g_svc) != 0) {
        SVC_LOG_ERROR("Failed to start channel service");
        channel_service_destroy(g_svc);
        g_svc = NULL;
        airy_mtx_destroy(&g_running_lock_channel_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("channel_d started (max_channels=%u, socket_dir=%s)", config.max_channels,
                 config.socket_dir);

    airy_sock_t server_fd = daemon_create_server_socket(use_tcp, CHANNEL_D_DEFAULT_PORT,
                                                        CHANNEL_D_SOCKET_PATH, CHANNEL_D_PIPE_PATH);
    if (server_fd < 0) {
        SVC_LOG_ERROR("channel_d: failed to create socket at %s", CHANNEL_D_SOCKET_PATH);
        channel_service_destroy(g_svc);
        g_svc = NULL;
        airy_mtx_destroy(&g_running_lock_channel_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO("channel_d: listening on %s (fd=%d)", CHANNEL_D_SOCKET_PATH, (int)server_fd);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = 4;
    ev_config.thread_pool_max = 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_channel_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = use_tcp ? "127.0.0.1" : CHANNEL_D_SOCKET_PATH;
    int ret = daemon_init_event_driver("channel_d", "channel", sock_addr,
                                       use_tcp ? CHANNEL_D_DEFAULT_PORT : 0, "channel,core",
                                       use_tcp, &ev_config, &g_event_driver_channel_d,
                                       &g_bsd_channel_d, &g_bipc_channel_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_channel_d) {
        SVC_LOG_ERROR("channel_d: failed to create event driver");
        airy_sock_close(server_fd);
        channel_service_destroy(g_svc);
        g_svc = NULL;
        airy_mtx_destroy(&g_running_lock_channel_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_channel_d = daemon_event_driver_get_dispatcher(g_event_driver_channel_d);
    method_dispatcher_register(g_dispatcher_channel_d, "ping", channel_on_ping, NULL);
    method_dispatcher_register(g_dispatcher_channel_d, "list", channel_on_list, NULL);
    method_dispatcher_register(g_dispatcher_channel_d, "open", channel_on_open, NULL);
    method_dispatcher_register(g_dispatcher_channel_d, "close", channel_on_close, NULL);
    method_dispatcher_register(g_dispatcher_channel_d, "send", channel_on_send, NULL);
    method_dispatcher_register(g_dispatcher_channel_d, "health", channel_on_health, NULL);

    method_dispatcher_register(g_dispatcher_channel_d, "health_check", channel_on_health, NULL);

    method_dispatcher_register(g_dispatcher_channel_d, "shutdown", on_shutdown_method_channel_d,
                               NULL);

    method_dispatcher_register(g_dispatcher_channel_d, "get_stats", channel_on_get_stats, NULL);
    SVC_LOG_INFO("channel_d: registered 9 RPC methods (channel.* namespace)");

    if (daemon_event_driver_add_server_fd(g_event_driver_channel_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("channel_d: failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_channel_d);
        airy_sock_close(server_fd);
        channel_service_destroy(g_svc);
        g_svc = NULL;
        airy_mtx_destroy(&g_running_lock_channel_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("channel_d: running (event-driven mode), waiting for requests");
    daemon_event_driver_run(g_event_driver_channel_d);

    SVC_LOG_INFO("channel_d shutting down");
    daemon_cleanup_standard(g_bipc_channel_d, g_bsd_channel_d, g_event_driver_channel_d, server_fd,
                            CHANNEL_D_SOCKET_PATH, destroy_service_channel_d,
                            &g_running_lock_channel_d);
    log_cleanup();
    return 0;
}
