// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file main.c
 * @brief Monitoring service daemon main entry (daemon module conventions).
 */

#include "daemon_main.h"
#include "info_rpc.h"
#include "monitor_service.h"
#include "observe_rpc.h"
#include "param_validator.h"
#include "platform.h"
#include "prometheus_exporter.h"
#include "svc_logger.h"
#include "thread_pool.h"

#include <stdlib.h>
#include <time.h>

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("monit.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_monit"
#define DEFAULT_TCP_PORT 9090
#define MAX_BUFFER 65536

DAEMON_DECLARE_COMMON(monit_d, monitor, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(monit_d)

static monitor_service_t *g_service = NULL;
static uint64_t g_service_start_time = 0;

#define MONIT_ERR_INVALID_PARAM AIRY_ERR_INVALID_PARAM
#define MONIT_ERR_OUT_OF_MEMORY AIRY_ERR_OUT_OF_MEMORY
#define MONIT_ERR_NOT_FOUND AIRY_ERR_NOT_FOUND
#define MONIT_ERR_INVALID_METRIC (AIRY_ERR_DAEMON_BASE + 0x10)
#define MONIT_ERR_ALERT_FAILED (AIRY_ERR_DAEMON_BASE + 0x11)

static void handle_record_metric(cJSON *params, int id, airy_sock_t client_fd);
static void handle_get_metrics(cJSON *params, int id, airy_sock_t client_fd);
static void handle_get_stats(int id, airy_sock_t client_fd);
static void free_alert_info_list(alert_info_t **alerts, size_t count);
static void handle_trigger_alert(cJSON *params, int id, airy_sock_t client_fd);
static void handle_get_alerts(int id, airy_sock_t client_fd);
static void handle_health_check(cJSON *params, int id, airy_sock_t client_fd);
static void handle_generate_report(int id, airy_sock_t client_fd);
static void handle_heartbeat(cJSON *params, int id, airy_sock_t client_fd);
static void handle_alert_resolve(cJSON *params, int id, airy_sock_t client_fd);
static void handle_client(airy_sock_t client_fd);

static void on_record_metric_method(cJSON *params, int id, void *user_data)
{
    handle_record_metric(params, id, *(airy_sock_t *)user_data);
}

static void on_get_metrics_method(cJSON *params, int id, void *user_data)
{
    handle_get_metrics(params, id, *(airy_sock_t *)user_data);
}

static void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

static void on_trigger_alert_method(cJSON *params, int id, void *user_data)
{
    handle_trigger_alert(params, id, *(airy_sock_t *)user_data);
}

static void on_get_alerts_method(cJSON *params, int id, void *user_data)
{
    handle_get_alerts(id, *(airy_sock_t *)user_data);
}

static void on_health_check_method(cJSON *params, int id, void *user_data)
{
    handle_health_check(params, id, *(airy_sock_t *)user_data);
}

static void on_generate_report_method(cJSON *params, int id, void *user_data)
{
    handle_generate_report(id, *(airy_sock_t *)user_data);
}

static void on_heartbeat_method(cJSON *params, int id, void *user_data)
{
    handle_heartbeat(params, id, *(airy_sock_t *)user_data);
}

static void on_alert_resolve_method(cJSON *params, int id, void *user_data)
{
    handle_alert_resolve(params, id, *(airy_sock_t *)user_data);
}

static int monit_on_client(void *service_ctx, airy_sock_t client_fd)
{
    (void)service_ctx;
    handle_client(client_fd);
    return 0;
}

static void monit_on_metrics_timer(airy_event_loop_t *loop, uint64_t timer_id, void *user_data)
{
    (void)loop;
    (void)timer_id;
    (void)user_data;

    uint64_t scrape_count = 0, scrape_errors = 0;
    prometheus_exporter_get_scrape_stats(&scrape_count, &scrape_errors);
    SVC_LOG_INFO("C-L10: Metrics report — scrapes=%llu errors=%llu",
                 (unsigned long long)scrape_count, (unsigned long long)scrape_errors);

    prometheus_gauge_set("airy_monit_scrape_count", (double)scrape_count);
    prometheus_gauge_set("airy_monit_scrape_errors", (double)scrape_errors);

    if (g_bipc_monit_d) {
        ipc_bus_helper_t *ibh = daemon_bootstrap_ipc_get_helper(g_bipc_monit_d);
        if (ibh) {
            uint64_t total_sends = 0, total_routes = 0, route_fallbacks = 0;
            uint64_t send_failures = 0, bp_drops = 0, bp_rejects = 0;
            if (ipc_bus_helper_get_routing_stats(ibh, &total_sends, &total_routes, &route_fallbacks,
                                                 &send_failures, &bp_drops, &bp_rejects) == 0) {
                if (total_sends > 0 || total_routes > 0) {
                    SVC_LOG_INFO("C-L10: IPC Bus — sends=%llu routes=%llu fallbacks=%llu "
                                 "failures=%llu bp_drops=%llu bp_rejects=%llu",
                                 (unsigned long long)total_sends, (unsigned long long)total_routes,
                                 (unsigned long long)route_fallbacks,
                                 (unsigned long long)send_failures, (unsigned long long)bp_drops,
                                 (unsigned long long)bp_rejects);
                }
            }
        }
    }
}

static void handle_record_metric(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *metric_json = jsonrpc_get_object_param(params, "metric");
    if (!metric_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing metric object", id);
        return;
    }

    metric_info_t metric = {0};
    const char *mname = get_string_field(metric_json, "name", NULL);
    if (!mname) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing metric name", id);
        return;
    }

    metric.name = AIRY_STRDUP(mname);
    metric.description = (char *)get_string_field(metric_json, "description", NULL);
    metric.type = (metric_type_t)get_int_field(metric_json, "type", 0);
    metric.value = get_double_field(metric_json, "value", 0.0);

    metric.timestamp = (uint64_t)time(NULL) * 1000;

    int ret = monitor_service_record_metric(g_service, &metric);

    AIRY_FREE((void *)metric.name);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Record metric failed", id);
        SVC_LOG_ERROR("Failed to record metric: %s (error=%d)", mname, ret);
    } else {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "recorded");
        cJSON_AddStringToObject(result, "metric_name", mname);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_DEBUG("Metric recorded: %s", mname);
    }
}

static void handle_get_metrics(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *filter = get_string_field(params, "metric_name", NULL);

    metric_info_t **metrics = NULL;
    size_t count = 0;
    int ret = monitor_service_get_metrics(g_service, filter, &metrics, &count);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Get metrics failed", id);
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count && metrics && metrics[i]; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "name", metrics[i]->name);
        if (metrics[i]->description)
            cJSON_AddStringToObject(m, "description", metrics[i]->description);
        cJSON_AddNumberToObject(m, "type", metrics[i]->type);
        cJSON_AddNumberToObject(m, "value", metrics[i]->value);
        cJSON_AddNumberToObject(m, "timestamp", (double)metrics[i]->timestamp);
        cJSON_AddItemToArray(arr, m);
    }

    AIRY_FREE(metrics);

    JSONRPC_SEND_SUCCESS(client_fd, arr, id);
}

static void handle_get_stats(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "daemon", "monit_d");
    cJSON_AddNumberToObject(result, "uptime_s",
                            (double)((uint64_t)time(NULL) - (uint64_t)g_service_start_time));
    if (g_service) {
        metric_info_t **metrics = NULL;
        size_t mcount = 0;
        if (monitor_service_get_metrics(g_service, NULL, &metrics, &mcount) == AIRY_SUCCESS) {
            cJSON_AddNumberToObject(result, "metrics", (double)mcount);
            AIRY_FREE(metrics);
        } else {
            cJSON_AddNumberToObject(result, "metrics", 0);
        }
        alert_info_t **alerts = NULL;
        size_t acount = 0;
        if (monitor_service_get_alerts(g_service, &alerts, &acount) == AIRY_SUCCESS) {
            int resolved = 0;
            for (size_t i = 0; i < acount && alerts && alerts[i]; i++) {
                if (alerts[i]->is_resolved)
                    resolved++;
            }
            cJSON_AddNumberToObject(result, "alerts", (double)acount);
            cJSON_AddNumberToObject(result, "alerts_resolved", (double)resolved);
            free_alert_info_list(alerts, acount);
        } else {
            cJSON_AddNumberToObject(result, "alerts", 0);
            cJSON_AddNumberToObject(result, "alerts_resolved", 0);
        }
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_trigger_alert(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *alert_json = jsonrpc_get_object_param(params, "alert");
    if (!alert_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing alert object", id);
        return;
    }

    alert_info_t alert = {0};
    alert.alert_id = (char *)get_string_field(alert_json, "alert_id", NULL);
    alert.message = (char *)get_string_field(alert_json, "message", NULL);
    alert.level = (alert_level_t)get_int_field(alert_json, "level", 0);
    alert.service_name = (char *)get_string_field(alert_json, "service_name", NULL);
    alert.resource_id = (char *)get_string_field(alert_json, "resource_id", NULL);

    alert.timestamp = (uint64_t)time(NULL) * 1000;
    alert.is_resolved = false;

    int ret = monitor_service_trigger_alert(g_service, &alert);
    const char *alert_id = alert.alert_id ? alert.alert_id : "unknown";

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Trigger alert failed", id);
        SVC_LOG_ERROR("Failed to trigger alert: %s (error=%d)", alert_id, ret);
    } else {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "triggered");
        if (alert.alert_id)
            cJSON_AddStringToObject(result, "alert_id", alert.alert_id);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_INFO("Alert triggered: %s", alert_id);
    }
}

static void free_alert_info_list(alert_info_t **alerts, size_t count)
{
    if (!alerts)
        return;
    for (size_t i = 0; i < count && alerts[i]; i++) {
        AIRY_FREE(alerts[i]->alert_id);
        AIRY_FREE(alerts[i]->message);
        AIRY_FREE(alerts[i]->service_name);
        AIRY_FREE(alerts[i]->resource_id);
        AIRY_FREE(alerts[i]);
    }
    AIRY_FREE(alerts);
}

static void handle_get_alerts(int id, airy_sock_t client_fd)
{
    alert_info_t **alerts = NULL;
    size_t count = 0;
    int ret = monitor_service_get_alerts(g_service, &alerts, &count);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Get alerts failed", id);
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count && alerts && alerts[i]; i++) {
        cJSON *a = cJSON_CreateObject();
        if (alerts[i]->alert_id)
            cJSON_AddStringToObject(a, "alert_id", alerts[i]->alert_id);
        if (alerts[i]->message)
            cJSON_AddStringToObject(a, "message", alerts[i]->message);
        cJSON_AddNumberToObject(a, "level", alerts[i]->level);
        if (alerts[i]->service_name)
            cJSON_AddStringToObject(a, "service_name", alerts[i]->service_name);
        cJSON_AddBoolToObject(a, "is_resolved", alerts[i]->is_resolved);
        cJSON_AddNumberToObject(a, "timestamp", (double)alerts[i]->timestamp);
        cJSON_AddItemToArray(arr, a);
    }

    free_alert_info_list(alerts, count);

    JSONRPC_SEND_SUCCESS(client_fd, arr, id);
}

static void handle_health_check(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *service_name = get_string_field(params, "service_name", "unknown");

    health_check_result_t *result = NULL;
    int ret = monitor_service_health_check(g_service, service_name, &result);

    if (ret != AIRY_SUCCESS || !result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Health check failed", id);
        return;
    }

    cJSON *res_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(res_obj, "service_name", result->service_name);
    cJSON_AddBoolToObject(res_obj, "healthy", result->is_healthy);
    if (result->status_message)
        cJSON_AddStringToObject(res_obj, "status_message", result->status_message);
    cJSON_AddNumberToObject(res_obj, "timestamp", (double)result->timestamp);

    JSONRPC_SEND_SUCCESS(client_fd, res_obj, id);

    AIRY_FREE(result->service_name);
    AIRY_FREE(result->status_message);
    AIRY_FREE(result);
}

static void handle_generate_report(int id, airy_sock_t client_fd)
{
    char *report = NULL;
    int ret = monitor_service_generate_report(g_service, &report);

    if (ret != AIRY_SUCCESS || !report) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Generate report failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "report", report);
    cJSON_AddNumberToObject(result, "generated_at", (double)(uint64_t)time(NULL) * 1000);
    AIRY_FREE(report);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_heartbeat(cJSON *params, int id, airy_sock_t client_fd)
{
    metric_info_t metric = {0};
    metric.name = AIRY_STRDUP("heartbeat");
    metric.description = (char *)get_string_field(params, "description", NULL);
    metric.type = METRIC_TYPE_COUNTER;
    metric.value = get_double_field(params, "value", 1.0);
    metric.timestamp = (uint64_t)time(NULL) * 1000;

    int ret = monitor_service_record_metric(g_service, &metric);
    AIRY_FREE((void *)metric.name);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Heartbeat record failed", id);
        SVC_LOG_ERROR("monit.heartbeat record failed: error=%d", ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "received", true);
    cJSON_AddStringToObject(result, "service", "monit_d");
    cJSON_AddNumberToObject(result, "timestamp", (double)metric.timestamp);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_DEBUG("Heartbeat recorded: timestamp=%llu", (unsigned long long)metric.timestamp);
}

static void handle_alert_resolve(cJSON *params, int id, airy_sock_t client_fd)
{
    const char *alert_id = get_string_field(params, "alert_id", NULL);
    if (!alert_id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing alert_id", id);
        return;
    }

    int ret = monitor_service_resolve_alert(g_service, alert_id);
    if (ret == AIRY_ERR_NOT_FOUND) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Alert not found", id);
        return;
    }
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Alert resolve failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "resolved", true);
    cJSON_AddStringToObject(result, "alert_id", alert_id);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("Alert resolved via RPC: %s", alert_id);
}

static void handle_client(airy_sock_t client_fd)
{
    char buffer[MAX_BUFFER];
#if AIRY_PLATFORM_POSIX
    /* Wait for request data before recv. The fd accepted by the event
     * driver may not have data yet; airy_sock_recv is a MSG_DONTWAIT
     * non-blocking read, so an immediate recv can return 0 on EAGAIN, be
     * mistaken for a failed connection and closed, causing SIGPIPE / lost
     * requests on the client send (RPC timing race). Poll for POLLIN first.
     * 5s timeout is ample vs the client daemon_rpc_call default of 30s; on
     * timeout the request is treated as lost (matches daemon_main.h). */
    struct pollfd pfd;
    pfd.fd = (int)client_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, 5000);
    if (pr <= 0 || !(pfd.revents & POLLIN)) {
        airy_sock_close(client_fd);
        return;
    }
#endif
    ssize_t n = airy_sock_recv(client_fd, buffer, sizeof(buffer) - 1);

    if (n <= 0) {
        airy_sock_close(client_fd);
        return;
    }
    buffer[n] = '\0';

    if ((size_t)n >= sizeof(buffer) - 1) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST, "Request too large", -1);
        airy_sock_close(client_fd);
        return;
    }

    char *http_response = NULL;
    size_t http_response_len = 0;
    /* /metrics 融合导出：动态指标表 + user-managed 指标（内部含 prometheus_exporter 通路） */
    if (obs_rpc_handle_http(buffer, (size_t)n, &http_response, &http_response_len) == 0) {
        airy_sock_send(client_fd, http_response, http_response_len);
        AIRY_FREE(http_response);
        airy_sock_close(client_fd);
        return;
    }

    CJSON_PARSE_GUARD(req, buffer, {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_PARSE_ERROR, "Parse error: invalid JSON", -1);
        airy_sock_close(client_fd);
        return;
    });

    cJSON *jsonrpc = cJSON_GetObjectItem(req, "jsonrpc");
    cJSON *method = cJSON_GetObjectItem(req, "method");
    (void)cJSON_GetObjectItem(req, "params");
    cJSON *id = cJSON_GetObjectItem(req, "id");

    if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0 ||
        !cJSON_IsString(method) || !id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST, "Invalid Request", -1);

        airy_sock_close(client_fd);
        return;
    }

    SVC_LOG_DEBUG("Processing request: method=%s, id=%d", method->valuestring,
                  cJSON_IsNumber(id) ? id->valueint : 0);

    method_dispatcher_dispatch(g_dispatcher_monit_d, req, jsonrpc_build_error, &client_fd);

    airy_sock_close(client_fd);
}

static void destroy_service(void)
{
    observe_rpc_cleanup();
    info_rpc_cleanup();
    prometheus_exporter_shutdown();
    if (g_service) {
        monitor_service_destroy(g_service);
        g_service = NULL;
    }
}

int main(int argc, char **argv)
{
    const char *config_path = "agentrt/manager/service/monit_d/monit.yaml";
    int use_tcp = 0;

    g_service_start_time = (uint64_t)time(NULL);

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_monit_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_monit_d);

#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler_monit_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(monit_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    daemon_cupolas_init_pep("monit_d");

    SVC_LOG_INFO("Monitor service starting, manager=%s", config_path);

    monitor_config_t config = {.metrics_collection_interval_ms = 5000,
                               .health_check_interval_ms = 10000,
                               .log_flush_interval_ms = 30000,
                               .alert_check_interval_ms = 5000,
                               .log_file_path = "monitor.log",
                               .metrics_storage_path = "metrics",
                               .enable_tracing = true,
                               .enable_alerting = true};

    int ret = monitor_service_create(&config, &g_service);
    if (ret != AIRY_SUCCESS || !g_service) {
        SVC_LOG_ERROR("Failed to create monitor service (error=%d)", ret);
        airy_mtx_destroy(&g_running_lock_monit_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("Monitor service created successfully");

    if (prometheus_exporter_init("monit_d") == 0) {
        int metrics_ret = prometheus_exporter_register_required_metrics();
        if (metrics_ret != 0) {
            SVC_LOG_WARN("C-L10: Some required metrics failed to register (ret=%d)", metrics_ret);
        }
    } else {
        SVC_LOG_ERROR("C-L10: Failed to initialize Prometheus exporter");
    }

    /* M4 整编：observe/info 作为 monit_d 内建模块初始化（失败仅降级，不阻断启动） */
    if (observe_rpc_init() != AIRY_SUCCESS) {
        SVC_LOG_ERROR("observe module init failed, observe_* methods unavailable");
    }
    if (info_rpc_init() != AIRY_SUCCESS) {
        SVC_LOG_ERROR("info module init failed, info_* methods unavailable");
    } else if (info_rpc_start() != AIRY_SUCCESS) {
        SVC_LOG_WARN("info collector thread not started, info history will be stale");
    }

    airy_sock_t server_fd =
        daemon_create_server_socket(use_tcp, DEFAULT_TCP_PORT, DEFAULT_SOCKET_PATH_UNIX,
                                    DEFAULT_SOCKET_PATH_WIN);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service();
        airy_mtx_destroy(&g_running_lock_monit_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO(use_tcp ? "Listening on TCP port %d" : "Listening on Unix socket",
                 DEFAULT_TCP_PORT);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = 2;
    ev_config.thread_pool_max = 4;
    ev_config.thread_pool_queue_size = 128;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = monit_on_client;
    ev_config.service_ctx = NULL;

    const char *sock_addr = use_tcp ? "127.0.0.1" : DEFAULT_SOCKET_PATH_UNIX;
    ret = daemon_init_event_driver("monit_d", "monitor", sock_addr, use_tcp ? DEFAULT_TCP_PORT : 0,
                                   "monitor,core", use_tcp, &ev_config, &g_event_driver_monit_d,
                                   &g_bsd_monit_d, &g_bipc_monit_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_monit_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service();
        airy_mtx_destroy(&g_running_lock_monit_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_monit_d = daemon_event_driver_get_dispatcher(g_event_driver_monit_d);
    method_dispatcher_register(g_dispatcher_monit_d, "record_metric", on_record_metric_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_monit_d, "get_metrics", on_get_metrics_method, NULL);
    method_dispatcher_register(g_dispatcher_monit_d, "trigger_alert", on_trigger_alert_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_monit_d, "get_alerts", on_get_alerts_method, NULL);
    method_dispatcher_register(g_dispatcher_monit_d, "health_check", on_health_check_method, NULL);
    method_dispatcher_register(g_dispatcher_monit_d, "generate_report", on_generate_report_method,
                               NULL);
    /* L2 protocol standard methods + aliases (02-l2-service-protocol.md:
     * monit.heartbeat / monit.metrics / monit.alert_raise / monit.alert_resolve)
     */
    method_dispatcher_register(g_dispatcher_monit_d, "heartbeat", on_heartbeat_method, NULL);
    method_dispatcher_register(g_dispatcher_monit_d, "metrics", on_get_metrics_method, NULL);
    method_dispatcher_register(g_dispatcher_monit_d, "alert_raise", on_trigger_alert_method, NULL);
    method_dispatcher_register(g_dispatcher_monit_d, "alert_resolve", on_alert_resolve_method,
                               NULL);
    /* L2 protocol standard method <ns>.shutdown (02-l2-service-protocol.md
     * §6.1: graceful stop, callable only by monit_d) */
    method_dispatcher_register(g_dispatcher_monit_d, "shutdown", on_shutdown_method_monit_d, NULL);

    method_dispatcher_register(g_dispatcher_monit_d, "get_stats", on_get_stats_method, NULL);
    observe_rpc_register(g_dispatcher_monit_d);
    info_rpc_register(g_dispatcher_monit_d);
    SVC_LOG_INFO("Registered %d RPC methods (monit.* namespace)", 19);

    daemon_event_driver_add_timer(g_event_driver_monit_d, 30000, monit_on_metrics_timer, NULL);
    SVC_LOG_INFO("C-L10: Metrics report timer registered (30s interval)");

    if (daemon_event_driver_add_server_fd(g_event_driver_monit_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_monit_d);
        airy_sock_close(server_fd);
        destroy_service();
        airy_mtx_destroy(&g_running_lock_monit_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("Monitor service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_monit_d);

    airy_time_sync_stop();

    daemon_cleanup_standard(g_bipc_monit_d, g_bsd_monit_d, g_event_driver_monit_d, server_fd,
                            DEFAULT_SOCKET_PATH_UNIX, destroy_service, &g_running_lock_monit_d);

    SVC_LOG_INFO("Monitor service stopped");
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
