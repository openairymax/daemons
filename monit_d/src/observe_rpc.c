// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file observe_rpc.c
 * @brief 动态指标观测域 RPC 服务（0.1.9 M4：observe_d → monit_d 整编）。
 *
 * observe_d（动态指标表 + Prometheus HTTP :9091）并入 monit_d（监控告警域）：
 * observe.record_metric / query_metrics / get_metrics 以 observe_* 前缀登记到
 * monit 调度器（monit 原生 record_metric/get_metrics 为嵌套 metric 对象语义，
 * 前缀消歧避免覆盖）；get_stats / health_check 走 monit 原生实现。
 *
 * /metrics 文本导出融合动态指标表与 unified-metrics 注册表
 * （prometheus_exporter），双 Prometheus 出口（:9091 与 monit TCP :9090）
 * 返回同一融合视图，达成 §5“双 Prometheus /metrics 收敛”。
 * 原独立 Unix/TCP server_fd、accept 循环与 raw 状态 JSON 兜底路径随
 * observe_d 消亡（消费方仅经 gateway JSON-RPC 转发访问）。
 */

#include "airy_memory.h"
#include "daemon_main.h"
#include "error.h"
#include "jsonrpc_helpers.h"
#include "method_dispatcher.h"
#include "observe_rpc.h"
#include "platform.h"
#include "prometheus_exporter.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

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

#define OBS_RPC_METRICS_PORT 9091
#define OBS_RPC_MAX_METRICS 256
#define OBS_RPC_MAX_BUFFER 65536
#define OBS_RPC_HTTP_BACKLOG 16

typedef struct {
    char *name;
    char *help;
    obs_metric_type_t type;
    double value;
    char *unit;
    uint64_t updated_at;
} obs_metric_t;

static airy_mtx_t g_obs_lock;
static obs_metric_t g_obs_metrics[OBS_RPC_MAX_METRICS];
static size_t g_obs_metric_count;
static uint64_t g_obs_start_time;
static uint64_t g_obs_requests;
static uint64_t g_obs_errors;
static uint64_t g_obs_http_requests;
static int g_obs_ready;

#ifndef _WIN32
static airy_sock_t g_obs_http_fd = AIRY_INVALID_SOCKET;
static airy_thread_t g_obs_http_thread;
static atomic_int g_obs_http_running;
static int g_obs_http_started;

static int obs_http_serve(airy_sock_t client)
{
    char buffer[OBS_RPC_MAX_BUFFER];
    ssize_t n = airy_sock_recv(client, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        airy_sock_close(client);
        return -1;
    }
    buffer[n] = '\0';

    airy_mtx_lock(&g_obs_lock);
    g_obs_http_requests++;
    uint64_t http_reqs = g_obs_http_requests;
    airy_mtx_unlock(&g_obs_lock);
    obs_rpc_record("airy_observe_http_requests_total", (double)http_reqs, NULL, OBS_GAUGE);

    char *response = NULL;
    size_t response_len = 0;
    if (obs_rpc_handle_http(buffer, (size_t)n, &response, &response_len) == 0) {
        airy_sock_send(client, response, response_len);
        AIRY_FREE(response);
    } else if (strstr(buffer, "GET /health") != NULL || strstr(buffer, "GET /healthz") != NULL) {
        uint64_t uptime = (uint64_t)time(NULL) - g_obs_start_time;
        char body[256];
        int blen = snprintf(body, sizeof(body),
                            "{\"status\":\"ok\",\"uptime_sec\":%llu,\"metrics_count\":%zu}",
                            (unsigned long long)uptime, obs_rpc_metric_count());
        char header[256];
        int hlen = snprintf(header, sizeof(header),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: %d\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            blen);
        airy_sock_send(client, header, (size_t)hlen);
        airy_sock_send(client, body, (size_t)blen);
    } else {
        static const char not_found[] = "HTTP/1.1 404 Not Found\r\n"
                                        "Content-Type: text/plain\r\n"
                                        "Content-Length: 9\r\n"
                                        "Connection: close\r\n"
                                        "\r\n"
                                        "Not Found";
        airy_sock_send(client, not_found, sizeof(not_found) - 1);
    }

    airy_sock_close(client);
    return 0;
}

static void *obs_http_loop(void *arg)
{
    (void)arg;
    while (atomic_load_explicit(&g_obs_http_running, memory_order_relaxed)) {
        airy_sock_t client = airy_sock_accept(g_obs_http_fd, 1000);
        if (client != AIRY_INVALID_SOCKET)
            obs_http_serve(client);
    }
    return NULL;
}

static void obs_start_http(void)
{
    g_obs_http_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_obs_http_fd == AIRY_INVALID_SOCKET) {
        SVC_LOG_WARN("observe_rpc: failed to create HTTP socket");
        return;
    }

    int reuse = 1;
    setsockopt(g_obs_http_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(OBS_RPC_METRICS_PORT);

    if (bind(g_obs_http_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(g_obs_http_fd, OBS_RPC_HTTP_BACKLOG) < 0) {
        /* 端口被占不阻断 monit_d 启动：观测域降级为纯 RPC 路径。 */
        SVC_LOG_WARN("observe_rpc: prometheus endpoint :%d unavailable (%s)",
                     OBS_RPC_METRICS_PORT, "bind/listen failed");
        airy_sock_close(g_obs_http_fd);
        g_obs_http_fd = AIRY_INVALID_SOCKET;
        return;
    }

    atomic_store_explicit(&g_obs_http_running, 1, memory_order_relaxed);
    if (airy_thread_create(&g_obs_http_thread, obs_http_loop, NULL) != 0) {
        SVC_LOG_WARN("observe_rpc: failed to start HTTP thread");
        atomic_store_explicit(&g_obs_http_running, 0, memory_order_relaxed);
        airy_sock_close(g_obs_http_fd);
        g_obs_http_fd = AIRY_INVALID_SOCKET;
        return;
    }
    g_obs_http_started = 1;
    SVC_LOG_INFO("observe_rpc: prometheus metrics endpoint started on :%d/metrics",
                 OBS_RPC_METRICS_PORT);
}

static void obs_stop_http(void)
{
    atomic_store_explicit(&g_obs_http_running, 0, memory_order_relaxed);
    if (g_obs_http_fd != AIRY_INVALID_SOCKET) {
        airy_sock_close(g_obs_http_fd);
        g_obs_http_fd = AIRY_INVALID_SOCKET;
    }
    if (g_obs_http_started) {
        airy_thread_join(g_obs_http_thread, NULL);
        g_obs_http_started = 0;
    }
}
#else
static void obs_start_http(void)
{
    SVC_LOG_WARN("observe_rpc: prometheus HTTP server not yet supported on Windows");
}
static void obs_stop_http(void)
{
}
#endif

/* 调用方持有 g_obs_lock。 */
static obs_metric_t *obs_find(const char *name)
{
    for (size_t i = 0; i < g_obs_metric_count; i++) {
        if (g_obs_metrics[i].name && strcmp(g_obs_metrics[i].name, name) == 0)
            return &g_obs_metrics[i];
    }
    if (g_obs_metric_count >= OBS_RPC_MAX_METRICS)
        return NULL;

    obs_metric_t *m = &g_obs_metrics[g_obs_metric_count];
    m->name = AIRY_STRDUP(name);
    m->help = AIRY_STRDUP(name);
    m->type = OBS_GAUGE;
    m->value = 0.0;
    m->unit = AIRY_STRDUP("count");
    m->updated_at = (uint64_t)time(NULL);
    g_obs_metric_count++;
    return m;
}

int obs_rpc_record(const char *name, double value, const char *unit, obs_metric_type_t type)
{
    if (!name)
        return AIRY_EINVAL;

    airy_mtx_lock(&g_obs_lock);
    obs_metric_t *m = obs_find(name);
    if (!m) {
        airy_mtx_unlock(&g_obs_lock);
        return AIRY_ENOMEM;
    }

    if (type == OBS_COUNTER)
        m->value += value;
    else
        m->value = value;

    m->type = type;
    m->updated_at = (uint64_t)time(NULL);
    if (unit && (!m->unit || strcmp(m->unit, unit) != 0)) {
        AIRY_FREE(m->unit);
        m->unit = AIRY_STRDUP(unit);
    }
    airy_mtx_unlock(&g_obs_lock);
    return 0;
}

int obs_rpc_format(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < 128)
        return AIRY_EINVAL;

    int off = 0;
    airy_mtx_lock(&g_obs_lock);
    for (size_t i = 0; i < g_obs_metric_count && off < (int)(buffer_size - 256); i++) {
        obs_metric_t *m = &g_obs_metrics[i];
        if (!m->name)
            continue;
        const char *type_str = m->type == OBS_COUNTER ? "counter" : "gauge";
        int added = snprintf(buffer + off, buffer_size - (size_t)off,
                             "# HELP %s %s\n"
                             "# TYPE %s %s\n"
                             "%s %.6f %llu\n",
                             m->name, m->help ? m->help : m->name, m->name, type_str, m->name,
                             m->value, (unsigned long long)(m->updated_at * 1000));
        if (added > 0)
            off += added;
    }
    airy_mtx_unlock(&g_obs_lock);
    return off;
}

size_t obs_rpc_metric_count(void)
{
    airy_mtx_lock(&g_obs_lock);
    size_t count = g_obs_metric_count;
    airy_mtx_unlock(&g_obs_lock);
    return count;
}

int obs_rpc_handle_http(const char *request, size_t request_len, char **response,
                        size_t *response_len)
{
    if (!request || !response || !response_len)
        return -1;
    if (strncmp(request, "GET /metrics", 12) != 0)
        return -1;

    char *dyn = AIRY_MALLOC(OBS_RPC_MAX_BUFFER);
    if (!dyn)
        return -1;
    int dyn_len = obs_rpc_format(dyn, OBS_RPC_MAX_BUFFER);
    if (dyn_len < 0)
        dyn_len = 0;

    /* unified-metrics 注册表文本经 exporter 渲染（保留其 scrape 统计）。 */
    char *um_resp = NULL;
    size_t um_len = 0;
    const char *um_body = NULL;
    size_t um_body_len = 0;
    if (prometheus_exporter_handle_http(request, request_len, &um_resp, &um_len) == 0 &&
        um_resp) {
        const char *sep = strstr(um_resp, "\r\n\r\n");
        if (sep) {
            um_body = sep + 4;
            um_body_len = um_len - (size_t)(um_body - um_resp);
        }
    }

    size_t total = (size_t)dyn_len + um_body_len;
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain; version=0.0.4\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        total);

    char *buf = AIRY_MALLOC((size_t)hlen + total + 1);
    if (!buf) {
        AIRY_FREE(dyn);
        AIRY_FREE(um_resp);
        return -1;
    }
    AIRY_MEMCPY(buf, header, (size_t)hlen);
    if (dyn_len > 0)
        AIRY_MEMCPY(buf + hlen, dyn, (size_t)dyn_len);
    if (um_body_len > 0)
        AIRY_MEMCPY(buf + hlen + dyn_len, um_body, um_body_len);

    AIRY_FREE(dyn);
    AIRY_FREE(um_resp);
    *response = buf;
    *response_len = (size_t)hlen + total;
    return 0;
}

/* ── RPC 处理器（自 observe_d main.c 移植，语义不变） ──────────────── */

static void obs_count(uint64_t *counter, const char *self_metric)
{
    airy_mtx_lock(&g_obs_lock);
    (*counter)++;
    airy_mtx_unlock(&g_obs_lock);
    obs_rpc_record(self_metric, (double)*counter, NULL, OBS_GAUGE);
}

static void obs_sync_self(void)
{
    uint64_t uptime = (uint64_t)time(NULL) - g_obs_start_time;
    obs_rpc_record("airy_observe_uptime_seconds", (double)uptime, "seconds", OBS_GAUGE);
    obs_rpc_record("airy_observe_metrics_count", (double)obs_rpc_metric_count(), "count",
                   OBS_GAUGE);
}

static void obs_h_record(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!cJSON_IsObject(params)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "params object required", id);
        return;
    }
    cJSON *namej = cJSON_GetObjectItem(params, "name");
    cJSON *valuej = cJSON_GetObjectItem(params, "value");
    cJSON *typej = cJSON_GetObjectItem(params, "type");
    cJSON *unitj = cJSON_GetObjectItem(params, "unit");
    if (!cJSON_IsString(namej) || !cJSON_IsNumber(valuej)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "name (string) and value (number) required", id);
        return;
    }
    obs_metric_type_t mtype = OBS_GAUGE;
    if (cJSON_IsString(typej)) {
        if (strcmp(typej->valuestring, "counter") == 0)
            mtype = OBS_COUNTER;
        else if (strcmp(typej->valuestring, "gauge") == 0)
            mtype = OBS_GAUGE;
        else {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                               "type must be gauge or counter", id);
            return;
        }
    }
    const char *unit = cJSON_IsString(unitj) ? unitj->valuestring : NULL;
    if (obs_rpc_record(namej->valuestring, valuej->valuedouble, unit, mtype) != 0) {
        obs_count(&g_obs_errors, "airy_observe_errors_total");
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "failed to record metric", id);
        return;
    }
    obs_count(&g_obs_requests, "airy_observe_requests_total");

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "recorded");
    cJSON_AddStringToObject(result, "name", namej->valuestring);
    cJSON_AddNumberToObject(result, "value", valuej->valuedouble);
    cJSON_AddStringToObject(result, "type", mtype == OBS_COUNTER ? "counter" : "gauge");
    if (unit)
        cJSON_AddStringToObject(result, "unit", unit);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void obs_h_query(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *filter = NULL;
    if (cJSON_IsObject(params)) {
        cJSON *namej = cJSON_GetObjectItem(params, "name");
        if (cJSON_IsString(namej))
            filter = namej->valuestring;
    }
    obs_sync_self();

    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    size_t count = 0;
    airy_mtx_lock(&g_obs_lock);
    for (size_t i = 0; i < g_obs_metric_count; i++) {
        obs_metric_t *mtr = &g_obs_metrics[i];
        if (!mtr->name)
            continue;
        if (filter && strcmp(mtr->name, filter) != 0)
            continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", mtr->name);
        cJSON_AddNumberToObject(item, "value", mtr->value);
        cJSON_AddStringToObject(item, "type", mtr->type == OBS_COUNTER ? "counter" : "gauge");
        cJSON_AddStringToObject(item, "unit", mtr->unit ? mtr->unit : "");
        cJSON_AddItemToArray(arr, item);
        count++;
    }
    airy_mtx_unlock(&g_obs_lock);
    cJSON_AddNumberToObject(result, "count", (double)count);
    cJSON_AddItemToObject(result, "metrics", arr);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

#define OBS_DECLARE(name, fn)                                                                   \
    static void obs_m_##name(cJSON *params, int id, void *user_data)                            \
    {                                                                                           \
        fn(params, id, *(airy_sock_t *)user_data);                                               \
    }

OBS_DECLARE(record_metric, obs_h_record)
OBS_DECLARE(query_metrics, obs_h_query)
OBS_DECLARE(get_metrics, obs_h_query)

int observe_rpc_init(void)
{
    if (g_obs_ready)
        return 0;

    __builtin_memset(g_obs_metrics, 0, sizeof(g_obs_metrics));
    g_obs_metric_count = 0;
    g_obs_requests = 0;
    g_obs_errors = 0;
    g_obs_http_requests = 0;
    g_obs_start_time = (uint64_t)time(NULL);
    airy_mtx_init(&g_obs_lock);
    g_obs_ready = 1;

    obs_rpc_record("airy_observe_requests_total", 0.0, "count", OBS_COUNTER);
    obs_rpc_record("airy_observe_errors_total", 0.0, "count", OBS_COUNTER);
    obs_rpc_record("airy_observe_http_requests_total", 0.0, "count", OBS_COUNTER);
    obs_rpc_record("airy_observe_metrics_count", 0.0, "count", OBS_GAUGE);
    obs_rpc_record("airy_observe_uptime_seconds", 0.0, "seconds", OBS_GAUGE);

    obs_start_http();
    SVC_LOG_INFO("observe_rpc: init complete (prometheus_port=%d)", OBS_RPC_METRICS_PORT);
    return 0;
}

void observe_rpc_cleanup(void)
{
    if (!g_obs_ready)
        return;

    obs_stop_http();
    airy_mtx_lock(&g_obs_lock);
    for (size_t i = 0; i < g_obs_metric_count; i++) {
        AIRY_FREE(g_obs_metrics[i].name);
        AIRY_FREE(g_obs_metrics[i].help);
        AIRY_FREE(g_obs_metrics[i].unit);
        g_obs_metrics[i].name = NULL;
        g_obs_metrics[i].help = NULL;
        g_obs_metrics[i].unit = NULL;
    }
    g_obs_metric_count = 0;
    airy_mtx_unlock(&g_obs_lock);
    airy_mtx_destroy(&g_obs_lock);
    g_obs_ready = 0;
    SVC_LOG_INFO("observe_rpc: cleaned up");
}

void observe_rpc_register(void *disp)
{
    method_dispatcher_t *d = (method_dispatcher_t *)disp;
    if (!d)
        return;
    /* observe_* 前缀登记：与 monit 原生 record_metric/get_metrics（嵌套
     * metric 对象语义）区分，避免覆盖 monit 语义；get_stats/health_check
     * 复用 monit 原生方法，不在此登记。 */
    method_dispatcher_register(d, "observe_record_metric", obs_m_record_metric, NULL);
    method_dispatcher_register(d, "observe_query_metrics", obs_m_query_metrics, NULL);
    method_dispatcher_register(d, "observe_get_metrics", obs_m_get_metrics, NULL);
    SVC_LOG_INFO("observe_rpc: registered observe.* methods on monit_d dispatcher "
                 "(observe_record_metric/query_metrics/get_metrics)");
}
