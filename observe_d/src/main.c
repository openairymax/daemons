// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (c) 2026 SPHARX. All Rights Reserved.
 * P0.18.1: daemon_main.h 传递性提供 atomic_compat、daemon_bootstrap_sd/ipc、
 * daemon_cupolas、daemon_platform_ext、logging、svc_logger 等头文件。
 * 本守护进程使用自定义 HTTP/Prometheus metrics 服务端与 accept 循环，
 * 不使用 DAEMON_DECLARE_COMMON 生成的 JSON-RPC 样板。
 */

#include "daemon_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define OBSERVE_D_DEFAULT_PORT 8085
#define OBSERVE_D_METRICS_PORT 9090
#define OBSERVE_D_MAX_BUFFER 65536
#define OBSERVE_D_DEFAULT_SOCKET AIRY_RUNTIME_DIR "/observe.sock"
#define OBSERVE_D_MAX_METRICS 256
#define OBSERVE_D_HTTP_BACKLOG 16

typedef enum { OBSERVE_METRIC_GAUGE, OBSERVE_METRIC_COUNTER } observe_metric_type_t;

typedef struct {
    char *name;
    char *help;
    observe_metric_type_t type;
    double value;
    char *unit;
    uint64_t updated_at;
} observe_metric_t;

typedef struct {
    airy_sock_t server_fd;
    airy_sock_t http_fd;
    airy_mtx_t lock;
    airy_thread_t http_thread;
    atomic_int running;
    atomic_int http_running;
    atomic_int force_stop;
    uint64_t start_time;
    uint64_t observe_count;
    uint64_t error_count;
    uint64_t http_request_count;
    observe_metric_t metrics[OBSERVE_D_MAX_METRICS];
    size_t metric_count;
    int tcp_port;
    int metrics_port;
    char *socket_path;
} observe_d_service_t;

static observe_d_service_t g_service = {0};
static atomic_int g_shutdown = 0;
static daemon_bootstrap_sd_t *g_bsd = NULL;
static daemon_bootstrap_ipc_t *g_bipc = NULL;

static void observe_d_signal_handler(int sig)
{

    atomic_store_explicit(&g_shutdown, 1, memory_order_seq_cst);
}

static observe_metric_t *observe_d_find_or_create_metric(observe_d_service_t *svc, const char *name)
{
    for (size_t i = 0; i < svc->metric_count; i++) {
        if (svc->metrics[i].name && strcmp(svc->metrics[i].name, name) == 0) {
            return &svc->metrics[i];
        }
    }

    if (svc->metric_count >= OBSERVE_D_MAX_METRICS) {
        AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
        }

    observe_metric_t *m = &svc->metrics[svc->metric_count];
    m->name = AIRY_STRDUP(name);
    m->help = AIRY_STRDUP(name);
    m->type = OBSERVE_METRIC_GAUGE;
    m->value = 0.0;
    m->unit = AIRY_STRDUP("count");
    m->updated_at = (uint64_t)time(NULL);
    svc->metric_count++;
    return m;
}

static int observe_d_record_metric(observe_d_service_t *svc, const char *name, double value,
                                   const char *unit, observe_metric_type_t type)
{
    if (!svc || !name) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "null svc or name");
    }

    airy_mtx_lock(&svc->lock);
    observe_metric_t *m = observe_d_find_or_create_metric(svc, name);
    if (!m) {
        airy_mtx_unlock(&svc->lock);
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "metric slot exhausted");
    }

    if (type == OBSERVE_METRIC_COUNTER)
        m->value += value;
    else
        m->value = value;

    m->type = type;
    m->updated_at = (uint64_t)time(NULL);
    if (unit) {
        AIRY_FREE(m->unit);
        m->unit = AIRY_STRDUP(unit);
    }
    airy_mtx_unlock(&svc->lock);
    return 0;
}

static int observe_d_format_prometheus(observe_d_service_t *svc, char *buffer, size_t buffer_size)
{
    if (!svc || !buffer || buffer_size < 128) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "null param or buffer too small");
    }

    int off = 0;
    airy_mtx_lock(&svc->lock);

    for (size_t i = 0; i < svc->metric_count && off < (int)(buffer_size - 256); i++) {
        observe_metric_t *m = &svc->metrics[i];
        if (!m->name)
            continue;

        const char *type_str = m->type == OBSERVE_METRIC_COUNTER ? "counter" : "gauge";

        int added = snprintf(buffer + off, buffer_size - (size_t)off,
                             "# HELP %s %s\n"
                             "# TYPE %s %s\n"
                             "%s %.6f %llu\n",
                             m->name, m->help ? m->help : m->name, m->name, type_str, m->name,
                             m->value, (unsigned long long)(m->updated_at * 1000));
        if (added > 0)
            off += added;
    }

    airy_mtx_unlock(&svc->lock);
    return off;
}

static int observe_d_handle_http_request(observe_d_service_t *svc, airy_sock_t client_fd)
{
    char buffer[OBSERVE_D_MAX_BUFFER];
    ssize_t n = airy_sock_recv(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        airy_sock_close(client_fd);
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "recv failed or connection closed");
    }
    buffer[n] = '\0';

    svc->http_request_count++;

    int is_metrics =
        (strstr(buffer, "GET /metrics") != NULL || strstr(buffer, "GET /metrics HTTP") != NULL);
    int is_health =
        (strstr(buffer, "GET /health") != NULL || strstr(buffer, "GET /healthz") != NULL);

    if (is_metrics) {
        char metrics_buf[65536];
        int metrics_len = observe_d_format_prometheus(svc, metrics_buf, sizeof(metrics_buf));

        char header[512];
        int header_len = snprintf(header, sizeof(header),
                                  "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/plain; version=0.0.4\r\n"
                                  "Content-Length: %d\r\n"
                                  "Connection: close\r\n"
                                  "\r\n",
                                  metrics_len > 0 ? metrics_len : 0);

        airy_sock_send(client_fd, header, (size_t)header_len);
        if (metrics_len > 0)
            airy_sock_send(client_fd, metrics_buf, (size_t)metrics_len);
    } else if (is_health) {
        uint64_t uptime = (uint64_t)time(NULL) - svc->start_time;
        char health_buf[512];
        int health_len = snprintf(health_buf, sizeof(health_buf),
                                  "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Content-Length: %d\r\n"
                                  "Connection: close\r\n"
                                  "\r\n"
                                  "{\"status\":\"ok\",\"uptime_sec\":%llu,\"metrics\":%zu}\r\n",
                                  0, (unsigned long long)uptime, svc->metric_count);

        int content_start = 0;
        for (int i = 0; i < health_len; i++) {
            if (health_buf[i] == '{') {
                content_start = i;
                break;
            }
        }
        (void)(health_len - content_start);

        char final_buf[1024];
        int final_len = snprintf(
            final_buf, sizeof(final_buf),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{\"status\":\"ok\",\"uptime_sec\":%llu,\"metrics_count\":%zu}",
            (int)snprintf(NULL, 0, "{\"status\":\"ok\",\"uptime_sec\":%llu,\"metrics_count\":%zu}",
                          (unsigned long long)uptime, svc->metric_count),
            (unsigned long long)uptime, svc->metric_count);

        airy_sock_send(client_fd, final_buf, (size_t)final_len);
    } else {
        const char *not_found = "HTTP/1.1 404 Not Found\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: 9\r\n"
                                "Connection: close\r\n"
                                "\r\n"
                                "Not Found";
        airy_sock_send(client_fd, not_found, strlen(not_found));
    }

    airy_sock_close(client_fd);
    return 0;
}

#ifdef _WIN32
static DWORD WINAPI observe_d_http_loop(LPVOID arg)
{
#else
static void *observe_d_http_loop(void *arg)
{
#endif
    observe_d_service_t *svc = (observe_d_service_t *)arg;
    if (!svc) {
#ifdef _WIN32
        return EXIT_FAILURE;
#else
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
#endif
    }

    while (svc->http_running) {
        airy_sock_t client = airy_sock_accept(svc->http_fd, 1000);
        if (client != AIRY_INVALID_SOCKET) {
            observe_d_handle_http_request(svc, client);
        }
    }

#ifdef _WIN32
    return 0;
#else
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
#endif
}

static int observe_d_init(observe_d_service_t *svc, int port, const char *sock)
{
    if (!svc)
        return AIRY_EINVAL;

    __builtin_memset(svc, 0, sizeof(*svc));
    svc->tcp_port = port > 0 ? port : OBSERVE_D_DEFAULT_PORT;
    svc->metrics_port = OBSERVE_D_METRICS_PORT;
    svc->socket_path = sock ? AIRY_STRDUP(sock) : AIRY_STRDUP(OBSERVE_D_DEFAULT_SOCKET);
    svc->start_time = (uint64_t)time(NULL);

    airy_mtx_init(&svc->lock);
    airy_sock_init();

    observe_d_record_metric(svc, "airy_observe_requests_total", 0.0, "count",
                            OBSERVE_METRIC_COUNTER);
    observe_d_record_metric(svc, "airy_observe_errors_total", 0.0, "count",
                            OBSERVE_METRIC_COUNTER);
    observe_d_record_metric(svc, "airy_observe_http_requests_total", 0.0, "count",
                            OBSERVE_METRIC_COUNTER);
    observe_d_record_metric(svc, "airy_observe_metrics_count", 0.0, "count",
                            OBSERVE_METRIC_GAUGE);
    observe_d_record_metric(svc, "airy_observe_uptime_seconds", 0.0, "seconds",
                            OBSERVE_METRIC_GAUGE);

    SVC_LOG_INFO("observe_d: init complete (prometheus_port=%d)", svc->metrics_port);
    return AIRY_SUCCESS;
}

static int observe_d_start_http_server(observe_d_service_t *svc)
{
#ifndef _WIN32
    svc->http_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (svc->http_fd == AIRY_INVALID_SOCKET) {
        SVC_LOG_ERROR("observe_d: failed to create HTTP socket");
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to create HTTP socket");
    }

    int reuse = 1;
    setsockopt(svc->http_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)svc->metrics_port);

    if (bind(svc->http_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        SVC_LOG_ERROR("observe_d: failed to bind HTTP port %d", svc->metrics_port);
        airy_sock_close(svc->http_fd);
        svc->http_fd = AIRY_INVALID_SOCKET;
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to bind HTTP port");
    }

    if (listen(svc->http_fd, OBSERVE_D_HTTP_BACKLOG) < 0) {
        SVC_LOG_ERROR("observe_d: failed to listen on HTTP port %d", svc->metrics_port);
        airy_sock_close(svc->http_fd);
        svc->http_fd = AIRY_INVALID_SOCKET;
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to listen on HTTP port");
    }

    svc->http_running = 1;
    airy_thread_create(&svc->http_thread, observe_d_http_loop, svc);

    SVC_LOG_INFO("observe_d: prometheus metrics endpoint started on :%d/metrics",
                 svc->metrics_port);
    return 0;
#else
    SVC_LOG_WARN("observe_d: prometheus HTTP server not yet supported on Windows");
    return 0;
#endif
}

static int observe_d_stop_http_server(observe_d_service_t *svc, int force)
{
    svc->http_running = 0;

    if (svc->http_fd != AIRY_INVALID_SOCKET) {
        airy_sock_close(svc->http_fd);
        svc->http_fd = AIRY_INVALID_SOCKET;
    }

    if (!force) {
        airy_thread_join(svc->http_thread, NULL);
    }

    SVC_LOG_INFO("observe_d: prometheus endpoint stopped (force=%d)", force);
    return 0;
}

static int observe_d_start(observe_d_service_t *svc)
{
    if (!svc) {
        AIRY_ERROR(AIRY_EINVAL, "null svc");
    }

#ifndef _WIN32
    svc->server_fd = airy_sock_create_unix_server(svc->socket_path);
    if (svc->server_fd < 0) {
        SVC_LOG_ERROR("observe_d: failed to create socket at %s", svc->socket_path);
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to create unix socket");
    }
#else
    svc->server_fd = airy_sock_create_tcp_server("127.0.0.1", (uint16_t)svc->tcp_port);
    if (svc->server_fd < 0) {
        SVC_LOG_ERROR("observe_d: failed to create TCP server");
        AIRY_ERROR(AIRY_ERR_UNKNOWN, "failed to create TCP server");
    }
#endif

    svc->running = 1;
    svc->force_stop = 0;

    observe_d_start_http_server(svc);

    SVC_LOG_INFO("observe_d: service started");
    return AIRY_SUCCESS;
}

static int observe_d_stop(observe_d_service_t *svc, int force)
{
    if (!svc)
        return AIRY_EINVAL;

    airy_mtx_lock(&svc->lock);
    svc->running = 0;
    if (force)
        svc->force_stop = 1;
    airy_mtx_unlock(&svc->lock);

    observe_d_stop_http_server(svc, force);

    if (svc->server_fd != AIRY_INVALID_SOCKET) {
        airy_sock_close(svc->server_fd);
        svc->server_fd = AIRY_INVALID_SOCKET;
    }

    if (force) {
#ifndef _WIN32
        unlink(svc->socket_path);
#endif
    }

    SVC_LOG_INFO("observe_d: service stopped (force=%d)", force);
    return AIRY_SUCCESS;
}

static int observe_d_destroy(observe_d_service_t *svc)
{
    if (!svc)
        return AIRY_EINVAL;

    if (svc->http_fd != AIRY_INVALID_SOCKET) {
        airy_sock_close(svc->http_fd);
        svc->http_fd = AIRY_INVALID_SOCKET;
    }

    for (size_t i = 0; i < svc->metric_count; i++) {
        AIRY_FREE(svc->metrics[i].name);
        AIRY_FREE(svc->metrics[i].help);
        AIRY_FREE(svc->metrics[i].unit);
    }
    if (svc->server_fd != AIRY_INVALID_SOCKET) {
        airy_sock_close(svc->server_fd);
    }
    airy_sock_cleanup();
    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc->socket_path);
    __builtin_memset(svc, 0, sizeof(*svc));
    SVC_LOG_INFO("observe_d: service destroyed");
    return AIRY_SUCCESS;
}

static int observe_d_healthcheck(observe_d_service_t *svc)
{
    if (!svc)
        return 0;
    if (!svc->running)
        return 0;

#ifndef _WIN32
    return (svc->http_fd != AIRY_INVALID_SOCKET && svc->http_running) ? 1 : 0;
#else
    return svc->running ? 1 : 0;
#endif
}

static void observe_d_handle_request(observe_d_service_t *svc, airy_sock_t client_fd)
{
    char buffer[OBSERVE_D_MAX_BUFFER];
    ssize_t n = airy_sock_recv(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        airy_sock_close(client_fd);
        return;
    }
    buffer[n] = '\0';

    airy_mtx_lock(&svc->lock);
    svc->observe_count++;
    airy_mtx_unlock(&svc->lock);

    observe_d_record_metric(svc, "airy_observe_requests_total", 1.0, "count",
                            OBSERVE_METRIC_COUNTER);

    int healthy = observe_d_healthcheck(svc);
    uint64_t uptime = (uint64_t)time(NULL) - svc->start_time;

    observe_d_record_metric(svc, "airy_observe_uptime_seconds", (double)uptime, "seconds",
                            OBSERVE_METRIC_GAUGE);

    airy_mtx_lock(&svc->lock);
    size_t mcount = svc->metric_count;
    airy_mtx_unlock(&svc->lock);

    char response[4096];
    snprintf(response, sizeof(response),
             "{"
             "\"service\":\"observe_d\","
             "\"status\":\"ok\","
             "\"observed\":%llu,"
             "\"metric_count\":%zu,"
             "\"http_requests\":%llu,"
             "\"uptime_sec\":%llu,"
             "\"healthy\":%s,"
             "\"prometheus\":{"
             "\"port\":%d,"
             "\"endpoint\":\"/metrics\""
             "}"
             "}",
             (unsigned long long)svc->observe_count, mcount,
             (unsigned long long)svc->http_request_count, (unsigned long long)uptime,
             healthy ? "true" : "false", svc->metrics_port);

    airy_sock_send(client_fd, response, strlen(response));
    airy_sock_close(client_fd);
}

int main(int argc __attribute__((unused)), char **argv __attribute__((unused)))
{

#ifndef _WIN32
    signal(SIGINT, observe_d_signal_handler);
    signal(SIGTERM, observe_d_signal_handler);
    signal(SIGPIPE, SIG_IGN);
#endif

    airy_log_init(NULL);
    atexit(airy_log_shutdown);

    /* P3.14 ACC-DT15: 初始化 cupolas 安全穹顶（permission_engine + sanitizer + audit_logger）*/
    daemon_cupolas_init("observe_d");

    if (observe_d_init(&g_service, OBSERVE_D_DEFAULT_PORT, OBSERVE_D_DEFAULT_SOCKET) !=
        AIRY_SUCCESS)
        return EXIT_FAILURE;
    if (observe_d_start(&g_service) != AIRY_SUCCESS) {
        observe_d_destroy(&g_service);
        return EXIT_FAILURE;
    }

    g_bsd = daemon_bootstrap_sd_start("observe_d", "observe", g_service.socket_path,
                                      0, "observe,core", 0);
    g_bipc = daemon_bootstrap_ipc_start("observe_d", "observe", g_service.socket_path,
                                        0, IPC_BUS_PROTO_JSON_RPC);

    while (!g_shutdown && g_service.running) {
        airy_sock_t client = airy_sock_accept(g_service.server_fd, 1000);
        if (client != AIRY_INVALID_SOCKET) {
            observe_d_handle_request(&g_service, client);
        }
    }

    daemon_bootstrap_ipc_stop(g_bipc);
    daemon_bootstrap_sd_stop(g_bsd);
    observe_d_stop(&g_service, g_shutdown ? 1 : 0);
    observe_d_destroy(&g_service);
    daemon_cupolas_cleanup(); /* P3.14 ACC-DT15: 清理 cupolas 安全穹顶 */
    return 0;
}
