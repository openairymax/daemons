// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_subsystems.c
 * @brief Monitor service 子系统域：指标记录/查询、日志记录、告警触发/解除、
 *        健康检查与文本报告生成。
 *
 * 2026-08-27 域拆分（原 service.c 900 行 → 3 文件）：生命周期与配置域见
 * service.c，Agent 执行追踪域见 service_agent_trace.c。
 */

#include "airy_memory.h"
#include "daemon_errors.h"
#include "monitor_service.h"
#include "daemon_platform_ext.h"
#include "monitor_service_internal.h"
#include "svc_logger.h"

#include <stdio.h>
#include <string.h>

int monitor_service_record_metric(monitor_service_t *service, const metric_info_t *metric)
{
    if (!service || !metric) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->metric_lock);

    if (service->metric_cache_count < MAX_METRICS) {
        metric_info_t *entry = (metric_info_t *)AIRY_CALLOC(1, sizeof(metric_info_t));
        if (!entry) {
            airy_mtx_unlock(&service->metric_lock);
            return AIRY_ENOMEM;
        }
        entry->name = metric->name ? AIRY_STRDUP(metric->name) : NULL;
        if (metric->name && !entry->name) {
            AIRY_FREE(entry);
            airy_mtx_unlock(&service->metric_lock);
            return AIRY_ENOMEM;
        }
        entry->description = metric->description ? AIRY_STRDUP(metric->description) : NULL;
        if (metric->description && !entry->description) {
            AIRY_FREE(entry->name);
            AIRY_FREE(entry);
            airy_mtx_unlock(&service->metric_lock);
            return AIRY_ENOMEM;
        }
        entry->type = metric->type;
        entry->value = metric->value;
        entry->timestamp = metric->timestamp ? metric->timestamp : get_timestamp_ms();
        service->metric_cache[service->metric_cache_count++] = entry;
    }

    airy_mtx_unlock(&service->metric_lock);
    return AIRY_SUCCESS;
}

int monitor_service_log(monitor_service_t *service, const log_info_t *log)
{
    if (!service || !log) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->log_lock);

    size_t idx = service->log_write_idx % MAX_LOG_ENTRIES;

    AIRY_FREE(service->logs[idx].message);
    AIRY_FREE(service->logs[idx].service_name);
    AIRY_FREE(service->logs[idx].file);
    AIRY_FREE(service->logs[idx].function);

    service->logs[idx].level = log->level;
    service->logs[idx].message = log->message ? AIRY_STRDUP(log->message) : NULL;
    service->logs[idx].service_name = log->service_name ? AIRY_STRDUP(log->service_name) : NULL;
    service->logs[idx].file = log->file ? AIRY_STRDUP(log->file) : NULL;
    service->logs[idx].line = log->line;
    service->logs[idx].function = log->function ? AIRY_STRDUP(log->function) : NULL;
    service->logs[idx].timestamp = log->timestamp ? log->timestamp : get_timestamp_ms();

    service->log_write_idx++;
    if (service->log_count < MAX_LOG_ENTRIES) {
        service->log_count++;
    }

    airy_mtx_unlock(&service->log_lock);
    return AIRY_SUCCESS;
}

int monitor_service_trigger_alert(monitor_service_t *service, const alert_info_t *alert)
{
    if (!service || !alert) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->alert_lock);

    if (service->alert_count >= MAX_ALERTS) {
        AIRY_FREE(service->alerts[0].alert_id);
        AIRY_FREE(service->alerts[0].message);
        AIRY_FREE(service->alerts[0].service_name);
        AIRY_FREE(service->alerts[0].resource_id);
        __builtin_memmove(&service->alerts[0], &service->alerts[1],
                          (service->alert_count - 1) * sizeof(alert_entry_t));
        service->alert_count--;
    }

    alert_entry_t *entry = &service->alerts[service->alert_count];
    entry->alert_id = alert->alert_id ? AIRY_STRDUP(alert->alert_id) : NULL;
    entry->message = alert->message ? AIRY_STRDUP(alert->message) : NULL;
    entry->level = alert->level;
    entry->service_name = alert->service_name ? AIRY_STRDUP(alert->service_name) : NULL;
    entry->resource_id = alert->resource_id ? AIRY_STRDUP(alert->resource_id) : NULL;
    entry->timestamp = alert->timestamp ? alert->timestamp : get_timestamp_ms();
    entry->is_resolved = false;
    service->alert_count++;

    airy_mtx_unlock(&service->alert_lock);

    const char *level_str[] = {"INFO", "WARNING", "ERROR", "CRITICAL"};
    SVC_LOG_WARN("Alert triggered: [%s] %s (service=%s)",
                 level_str[alert->level < ALERT_LEVEL_COUNT ? alert->level : 0],
                 alert->message ? alert->message : "N/A",
                 alert->service_name ? alert->service_name : "N/A");
    return AIRY_SUCCESS;
}

int monitor_service_resolve_alert(monitor_service_t *service, const char *alert_id)
{
    if (!service || !alert_id) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->alert_lock);

    for (size_t i = 0; i < service->alert_count; i++) {
        if (service->alerts[i].alert_id && strcmp(service->alerts[i].alert_id, alert_id) == 0) {
            service->alerts[i].is_resolved = true;
            airy_mtx_unlock(&service->alert_lock);
            SVC_LOG_INFO("Alert resolved: %s", alert_id);
            return AIRY_SUCCESS;
        }
    }

    airy_mtx_unlock(&service->alert_lock);
    AIRY_ERROR(AIRY_ERR_NOT_FOUND, "alert not found");
}

int monitor_service_health_check(monitor_service_t *service, const char *service_name,
                                 health_check_result_t **result)
{
    if (!service || !result) {
        return AIRY_ERR_INVALID_PARAM;
    }

    health_check_result_t *hr =
        (health_check_result_t *)AIRY_CALLOC(1, sizeof(health_check_result_t));
    if (!hr) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate health check result");
    }

    hr->service_name = service_name ? AIRY_STRDUP(service_name) : AIRY_STRDUP("monitor_service");
    hr->is_healthy = service->initialized ? true : false;
    hr->timestamp = get_timestamp_ms();
    hr->error_code = 0;

    airy_mtx_lock(&service->alert_lock);
    size_t unresolved_critical = 0;
    for (size_t i = 0; i < service->alert_count; i++) {
        if (!service->alerts[i].is_resolved && service->alerts[i].level >= ALERT_LEVEL_ERROR) {
            unresolved_critical++;
        }
    }
    airy_mtx_unlock(&service->alert_lock);

    if (unresolved_critical > 5) {
        hr->is_healthy = false;
        hr->status_message = AIRY_STRDUP("Too many unresolved critical alerts");
        hr->error_code = 1;
    } else if (unresolved_critical > 0) {
        hr->status_message = AIRY_STRDUP("Some unresolved alerts present");
    } else {
        hr->status_message = AIRY_STRDUP("Healthy");
    }

    *result = hr;
    return AIRY_SUCCESS;
}

int monitor_service_get_metrics(monitor_service_t *service, const char *metric_name,
                                metric_info_t ***metrics, size_t *count)
{
    if (!service || !metrics || !count) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->metric_lock);

    size_t result_count = 0;
    for (size_t i = 0; i < service->metric_cache_count; i++) {
        if (!metric_name || (service->metric_cache[i]->name &&
                             strstr(service->metric_cache[i]->name, metric_name))) {
            result_count++;
        }
    }

    if (result_count == 0) {
        *metrics = NULL;
        *count = 0;
        airy_mtx_unlock(&service->metric_lock);
        return AIRY_SUCCESS;
    }

    metric_info_t **result = (metric_info_t **)AIRY_CALLOC(result_count, sizeof(metric_info_t *));
    if (!result) {
        airy_mtx_unlock(&service->metric_lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate metrics result array");
    }

    size_t idx = 0;
    for (size_t i = 0; i < service->metric_cache_count && idx < result_count; i++) {
        if (!metric_name || (service->metric_cache[i]->name &&
                             strstr(service->metric_cache[i]->name, metric_name))) {
            result[idx++] = service->metric_cache[i];
        }
    }

    *metrics = result;
    *count = result_count;

    airy_mtx_unlock(&service->metric_lock);
    return AIRY_SUCCESS;
}

int monitor_service_get_alerts(monitor_service_t *service, alert_info_t ***alerts, size_t *count)
{
    if (!service || !alerts || !count) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->alert_lock);

    if (service->alert_count == 0) {
        *alerts = NULL;
        *count = 0;
        airy_mtx_unlock(&service->alert_lock);
        return AIRY_SUCCESS;
    }

    alert_info_t **result =
        (alert_info_t **)AIRY_CALLOC(service->alert_count, sizeof(alert_info_t *));
    if (!result) {
        airy_mtx_unlock(&service->alert_lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate alerts result array");
    }

    for (size_t i = 0; i < service->alert_count; i++) {
        alert_info_t *info = (alert_info_t *)AIRY_CALLOC(1, sizeof(alert_info_t));
        if (info) {
            /* Deep-copy the string fields: the original implementation
             * pointed directly into internal entries; after the lock is
             * released, the caller reading them races with concurrent
             * trigger_alert eviction (free) into a use-after-free. The caller
             * must release them too (AIRY_FREE each string + the element). */
            info->alert_id =
                service->alerts[i].alert_id ? AIRY_STRDUP(service->alerts[i].alert_id) : NULL;
            info->message =
                service->alerts[i].message ? AIRY_STRDUP(service->alerts[i].message) : NULL;
            info->service_name = service->alerts[i].service_name ?
                                     AIRY_STRDUP(service->alerts[i].service_name) :
                                     NULL;
            info->resource_id =
                service->alerts[i].resource_id ? AIRY_STRDUP(service->alerts[i].resource_id) : NULL;
            info->level = service->alerts[i].level;
            info->timestamp = service->alerts[i].timestamp;
            info->is_resolved = service->alerts[i].is_resolved;
        }
        result[i] = info;
    }

    *alerts = result;
    *count = service->alert_count;

    airy_mtx_unlock(&service->alert_lock);
    return AIRY_SUCCESS;
}

int monitor_service_generate_report(monitor_service_t *service, char **report)
{
    if (!service || !report) {
        return AIRY_ERR_INVALID_PARAM;
    }

    char *buf = (char *)AIRY_MALLOC(MAX_REPORT_SIZE);
    if (!buf) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate report buffer");
    }

    size_t pos = 0;
    pos += snprintf(buf + pos, MAX_REPORT_SIZE - pos,
                    "=== AgentRT Monitor Report ===\n"
                    "Generated at: %llu\n\n",
                    (unsigned long long)get_timestamp_ms());

    airy_mtx_lock(&service->metric_lock);
    pos += snprintf(buf + pos, MAX_REPORT_SIZE - pos, "--- Metrics (%zu recorded) ---\n",
                    service->metric_cache_count);
    for (size_t i = 0; i < service->metric_cache_count && pos < MAX_REPORT_SIZE - 256; i++) {
        metric_info_t *m = service->metric_cache[i];
        if (m && m->name) {
            pos += snprintf(buf + pos, MAX_REPORT_SIZE - pos, "  %s: %.4f (type=%d, ts=%llu)\n",
                            m->name, m->value, m->type, (unsigned long long)m->timestamp);
        }
    }
    airy_mtx_unlock(&service->metric_lock);

    airy_mtx_lock(&service->alert_lock);
    size_t unresolved = 0;
    for (size_t i = 0; i < service->alert_count; i++) {
        if (!service->alerts[i].is_resolved)
            unresolved++;
    }
    pos +=
        snprintf(buf + pos, MAX_REPORT_SIZE - pos, "\n--- Alerts (%zu total, %zu unresolved) ---\n",
                 service->alert_count, unresolved);
    const char *level_str[] = {"INFO", "WARNING", "ERROR", "CRITICAL"};
    for (size_t i = 0; i < service->alert_count && pos < MAX_REPORT_SIZE - 256; i++) {
        alert_entry_t *a = &service->alerts[i];
        pos += snprintf(buf + pos, MAX_REPORT_SIZE - pos, "  [%s] %s: %s%s\n",
                        level_str[a->level < ALERT_LEVEL_COUNT ? a->level : 0],
                        a->alert_id ? a->alert_id : "N/A", a->message ? a->message : "N/A",
                        a->is_resolved ? " (resolved)" : "");
    }
    airy_mtx_unlock(&service->alert_lock);

    airy_mtx_lock(&service->trace_lock);
    pos += snprintf(buf + pos, MAX_REPORT_SIZE - pos, "\n--- Traces (%zu recorded) ---\n",
                    service->trace_count);
    for (size_t i = 0; i < service->trace_count && pos < MAX_REPORT_SIZE - 256; i++) {
        trace_entry_t *t = &service->traces[i];
        pos += snprintf(buf + pos, MAX_REPORT_SIZE - pos, "  %s: %s (status=%d, duration=%llums)\n",
                        t->trace_id ? t->trace_id : "N/A",
                        t->operation_name ? t->operation_name : "N/A", t->status,
                        (unsigned long long)(t->end_time - t->start_time));
    }
    airy_mtx_unlock(&service->trace_lock);

    pos += snprintf(buf + pos, MAX_REPORT_SIZE - pos, "\n--- Logs (%zu entries) ---\n",
                    service->log_count);

    *report = buf;
    return AIRY_SUCCESS;
}
