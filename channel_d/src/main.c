// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (c) 2026 SPHARX. All Rights Reserved.
 * @file main.c
 * @brief Channel 守护进程主入口（P0.18.1 样板宏化）
 */

#include "channel_service.h"
#include "daemon_main.h"

#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>

#define CHANNEL_D_SOCKET_PATH AIRY_RUNTIME_DIR "/channel.sock"
#define CHANNEL_D_PIPE_PATH   "\\\\.\\pipe\\airy_channel"

/* P0.18.1: 使用 DAEMON_DECLARE_COMMON 生成公共样板（信号处理/全局变量/print_usage） */
DAEMON_DECLARE_COMMON(channel_d, channel, CHANNEL_D_SOCKET_PATH,
                      CHANNEL_D_PIPE_PATH, 0, 65536)

static channel_service_t *g_svc __attribute__((unused)) = NULL;

/* 销毁服务（daemon_cleanup_standard 回调） */
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

/* ==================== 业务逻辑：请求处理 ==================== */

__attribute__((used)) static int handle_service_request(const char *method, const char *params_json,
                                                        char **response_json, void *user_data)
{
    channel_service_t *svc = (channel_service_t *)user_data;
    if (!svc || !method || !response_json) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    if (strcmp(method, "ping") == 0) {
        const char *id_start = strstr(params_json, "\"id\"");
        if (!id_start) {
            bool healthy = channel_service_is_healthy(svc);
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"status\":\"%s\"}", healthy ? "ok" : "degraded");
            *response_json = AIRY_STRDUP(buf);
            return 0;
        }
        char id[128] = {0};
        const char *p = strchr(id_start + 4, '"');
        if (p) {
            p++;
            const char *e = strchr(p, '"');
            if (e) {
                size_t l = (size_t)(e - p);
                if (l > 127)
                    l = 127;
                __builtin_memcpy(id, p, l);
            }
        }
        int64_t latency_ms = 0;
        int rc = channel_service_ping(svc, id, &latency_ms);
        if (rc != CHANNEL_OK) {
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
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "malloc failed for ping response buffer");
        }
        snprintf(buf, sz, "{\"status\":\"ok\",\"channel_id\":\"%s\",\"latency_ms\":%lld}", id,
                 (long long)latency_ms);
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

    if (strcmp(method, "open") == 0) {
        const char *id_start = strstr(params_json, "\"id\"");
        const char *name_start = strstr(params_json, "\"name\"");
        const char *type_start = strstr(params_json, "\"type\"");

        if (!id_start || !name_start) {
            *response_json = AIRY_STRDUP("{\"error\":\"missing id or name\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "missing id or name in open request");
        }

        char id[128] = {0}, name[256] = {0};
        const char *p = strchr(id_start + 4, '"');
        if (p) {
            p++;
            const char *e = strchr(p, '"');
            if (e) {
                size_t l = (size_t)(e - p);
                if (l > 127)
                    l = 127;
                __builtin_memcpy(id, p, l);
            }
        }

        p = strchr(name_start + 6, '"');
        if (p) {
            p++;
            const char *e = strchr(p, '"');
            if (e) {
                size_t l = (size_t)(e - p);
                if (l > 255)
                    l = 255;
                __builtin_memcpy(name, p, l);
            }
        }

        channel_type_t type = CHANNEL_TYPE_SOCKET;
        if (type_start) {
            p = strchr(type_start + 6, ':');
            if (p) {
                int t = (int)strtol(p + 1, NULL, 10);
                if (t >= 0 && t <= 2)
                    type = (channel_type_t)t;
            }
        }

        int rc = channel_service_open(svc, id, name, type, NULL);
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
        const char *id_start = strstr(params_json, "\"id\"");
        if (!id_start) {
            *response_json = AIRY_STRDUP("{\"error\":\"missing id\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "missing id in close request");
        }
        char id[128] = {0};
        const char *p = strchr(id_start + 4, '"');
        if (p) {
            p++;
            const char *e = strchr(p, '"');
            if (e) {
                size_t l = (size_t)(e - p);
                if (l > 127)
                    l = 127;
                __builtin_memcpy(id, p, l);
            }
        }

        int rc = channel_service_close(svc, id);
        if (rc != 0) {
            *response_json = AIRY_STRDUP("{\"error\":\"close failed\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_close failed");
        }
        *response_json = AIRY_STRDUP("{\"status\":\"closed\"}");
        return 0;
    }

    if (strcmp(method, "send") == 0) {
        const char *id_start = strstr(params_json, "\"id\"");
        const char *data_start = strstr(params_json, "\"data\"");
        if (!id_start || !data_start) {
            *response_json = AIRY_STRDUP("{\"error\":\"missing id or data\"}");
            AIRY_ERROR(AIRY_ERR_UNKNOWN, "missing id or data in send request");
        }
        char id[128] = {0};
        const char *p = strchr(id_start + 4, '"');
        if (p) {
            p++;
            const char *e = strchr(p, '"');
            if (e) {
                size_t l = (size_t)(e - p);
                if (l > 127)
                    l = 127;
                __builtin_memcpy(id, p, l);
            }
        }

        p = strchr(data_start + 6, '"');
        if (p) {
            p++;
            const char *e = strchr(p, '"');
            size_t dlen = e ? (size_t)(e - p) : strlen(p);
            int rc = channel_service_send(svc, id, p, dlen);
            if (rc != 0) {
                char err[256];
                snprintf(err, sizeof(err), "{\"error\":\"send failed: %d\"}", rc);
                *response_json = AIRY_STRDUP(err);
                AIRY_ERROR(AIRY_ERR_UNKNOWN, "channel_service_send failed");
            }
        }
        *response_json = AIRY_STRDUP("{\"status\":\"sent\"}");
        return 0;
    }

    if (strcmp(method, "health") == 0) {
        bool healthy = channel_service_is_healthy(svc);
        *response_json = AIRY_STRDUP(healthy ? "{\"healthy\":true}" : "{\"healthy\":false}");
        return 0;
    }

    *response_json = AIRY_STRDUP("{\"error\":\"unknown method\"}");
    AIRY_ERROR(AIRY_ERR_UNKNOWN, "unknown method");
}

/* ==================== 主入口 ==================== */

int main(int argc, char *argv[])
{
    const char *socket_dir = NULL;
    uint32_t max_channels = CHANNEL_MAX_CHANNELS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            socket_dir = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_channels = (uint32_t)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-h") == 0) {
            fputs("Usage: channel_d [-c config] [-s socket_dir] [-n max_channels] [-h]\n", stdout);
            return 0;
        }
    }

    /* 跨平台信号处理 */
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
        return EXIT_FAILURE;
    }

    if (channel_service_start(g_svc) != 0) {
        SVC_LOG_ERROR("Failed to start channel service");
        channel_service_destroy(g_svc);
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("channel_d started (max_channels=%u, socket_dir=%s)", config.max_channels,
                 config.socket_dir);

    /* 创建 Socket 服务器 */
    airy_sock_t server_fd =
        daemon_create_server_socket(0, 0, CHANNEL_D_SOCKET_PATH, CHANNEL_D_PIPE_PATH);
    if (server_fd < 0) {
        SVC_LOG_ERROR("channel_d: failed to create socket at %s", CHANNEL_D_SOCKET_PATH);
        channel_service_destroy(g_svc);
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO("channel_d: listening on %s (fd=%d)", CHANNEL_D_SOCKET_PATH, (int)server_fd);

    g_bsd_channel_d = daemon_bootstrap_sd_start("channel_d", "channel", CHANNEL_D_SOCKET_PATH,
                                                  0, "channel,core", 0);
    g_bipc_channel_d = daemon_bootstrap_ipc_start("channel_d", "channel", CHANNEL_D_SOCKET_PATH,
                                                   0, IPC_BUS_PROTO_JSON_RPC);

    while (atomic_load_explicit(&g_running_channel_d, memory_order_acquire)) {
        /* 替代 sleep(1)，允许更快响应关闭信号 */
        for (int _w = 0; _w < 10 && atomic_load_explicit(&g_running_channel_d, memory_order_acquire); _w++) {
            usleep(100000); /* 100ms */
        }
    }

    SVC_LOG_INFO("channel_d shutting down");
    daemon_cleanup_standard(g_bipc_channel_d, g_bsd_channel_d, NULL, server_fd,
                            destroy_service_channel_d, &g_running_lock_channel_d);
    log_cleanup();
    return 0;
}
