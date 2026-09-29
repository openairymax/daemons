// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file monit_rpc.c
 * @brief monit.* RPC 方法域：指标记录 / 查询 / 告警触发与解除 /
 *        健康检查 / 报表导出 / 心跳 / 服务统计。
 *
 * 由 main.c 按单一职责拆分（0.1.19 gen5 装配）：method_fn 薄壳 m_*
 * 把机制层注入的 &client_fd 翻译为协议无关的 handler（cJSON*, int,
 * airy_sock_t）。L2 协议别名 metrics->get_metrics、alert_raise->
 * trigger_alert（02-l2-service-protocol.md）。
 */

#include "airy_memory.h"
#include "error.h"
#include "monit_d_internal.h"
#include "svc_monit_d.h"

#include "jsonrpc_helpers.h"
#include "param_validator.h"
#include "svc_logger.h"

#include <time.h>

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

void m_record_metric(cJSON *params, int id, void *user_data)
{
    handle_record_metric(params, id, *(airy_sock_t *)user_data);
}

void m_get_metrics(cJSON *params, int id, void *user_data)
{
    handle_get_metrics(params, id, *(airy_sock_t *)user_data);
}

void m_trigger_alert(cJSON *params, int id, void *user_data)
{
    handle_trigger_alert(params, id, *(airy_sock_t *)user_data);
}

void m_get_alerts(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_get_alerts(id, *(airy_sock_t *)user_data);
}

void m_health_check(cJSON *params, int id, void *user_data)
{
    handle_health_check(params, id, *(airy_sock_t *)user_data);
}

void m_generate_report(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_generate_report(id, *(airy_sock_t *)user_data);
}

void m_heartbeat(cJSON *params, int id, void *user_data)
{
    handle_heartbeat(params, id, *(airy_sock_t *)user_data);
}

/* L2 协议别名：monit.metrics == get_metrics；monit.alert_raise ==
 * trigger_alert（02-l2-service-protocol.md）。 */
void m_metrics(cJSON *params, int id, void *user_data)
{
    handle_get_metrics(params, id, *(airy_sock_t *)user_data);
}

void m_alert_raise(cJSON *params, int id, void *user_data)
{
    handle_trigger_alert(params, id, *(airy_sock_t *)user_data);
}

void m_alert_resolve(cJSON *params, int id, void *user_data)
{
    handle_alert_resolve(params, id, *(airy_sock_t *)user_data);
}

void m_get_stats(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_get_stats(id, *(airy_sock_t *)user_data);
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
