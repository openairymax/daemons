// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_agent_trace.c
 * @brief Monitor service Agent 执行追踪域：trace 启停/状态推进/循环检测/
 *        导出/汇总/活跃 Agent 列表。
 *
 * 2026-08-27 域拆分（原 service.c 900 行 → 3 文件）：生命周期与配置域见
 * service.c，指标/日志/告警/健康/报告域见 service_subsystems.c。
 */

#include "airy_memory.h"
#include "daemon_errors.h"
#include "monitor_service.h"
#include "daemon_platform_ext.h"
#include "monitor_service_internal.h"
#include "svc_logger.h"

#include <stdio.h>
#include <string.h>

int monitor_service_start_agent_trace(monitor_service_t *service,
                                      const char *agent_id __attribute__((unused)),
                                      const char *task_id __attribute__((unused)),
                                      const loop_detection_config_t *loop_config
                                      __attribute__((unused)),
                                      agent_execution_trace_t **trace)
{
    if (!service || !trace) {
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!service->config.enable_tracing) {
        AIRY_ERROR(AIRY_ERR_STATE_ERROR, "tracing is disabled");
    }

    airy_mtx_lock(&service->trace_lock);

    if (service->trace_count >= MAX_TRACES) {
        AIRY_FREE(service->traces[0].trace_id);
        AIRY_FREE(service->traces[0].operation_name);
        AIRY_FREE(service->traces[0].service_name);
        __builtin_memmove(&service->traces[0], &service->traces[1],
                          (service->trace_count - 1) * sizeof(trace_entry_t));
        service->trace_count--;
    }

    trace_entry_t *entry = &service->traces[service->trace_count];
    char tid[64];
    snprintf(tid, sizeof(tid), "trace-%zu-%lu", service->trace_count,
             (unsigned long)get_timestamp_ms());
    entry->trace_id = AIRY_STRDUP(tid);
    if (!entry->trace_id) {
        airy_mtx_unlock(&service->trace_lock);
        return AIRY_ENOMEM;
    }
    entry->operation_name = task_id ? AIRY_STRDUP(task_id) : AIRY_STRDUP("unknown");
    if (!entry->operation_name) {
        AIRY_FREE(entry->trace_id);
        airy_mtx_unlock(&service->trace_lock);
        return AIRY_ENOMEM;
    }
    entry->service_name = agent_id ? AIRY_STRDUP(agent_id) : NULL;
    if (agent_id && !entry->service_name) {
        AIRY_FREE(entry->operation_name);
        AIRY_FREE(entry->trace_id);
        airy_mtx_unlock(&service->trace_lock);
        return AIRY_ENOMEM;
    }
    entry->start_time = get_timestamp_ms();
    entry->end_time = 0;
    entry->status = 0;
    entry->span_count = 0;
    service->trace_count++;

    agent_execution_trace_t *t =
        (agent_execution_trace_t *)AIRY_CALLOC(1, sizeof(agent_execution_trace_t));
    if (t) {
        t->agent_id = agent_id ? AIRY_STRDUP(agent_id) : NULL;
        if (agent_id && !t->agent_id) {
            AIRY_FREE(t);
            t = NULL;
        } else {
            t->task_id = task_id ? AIRY_STRDUP(task_id) : NULL;
            if (task_id && !t->task_id) {
                AIRY_FREE(t->agent_id);
                AIRY_FREE(t);
                t = NULL;
            }
        }
    }
    if (t) {
        /* Backfill trace_id: the external trace handle must carry the
         * internally generated tid; otherwise end_agent_trace's trace_id
         * matching never hits, end_time stays 0, and get_active_agents always
         * reports ended traces as active (original defect). */
        t->trace_id = AIRY_STRDUP(tid);
        if (!t->trace_id) {
            AIRY_FREE(t->agent_id);
            AIRY_FREE(t->task_id);
            AIRY_FREE(t);
            t = NULL;
        } else {
            t->current_state = AGENT_STATE_INITIALIZING;
        }
    }

    *trace = t;
    airy_mtx_unlock(&service->trace_lock);
    return AIRY_SUCCESS;
}

int monitor_service_update_agent_state(monitor_service_t *service, agent_execution_trace_t *trace,
                                       agent_execution_state_t new_state, const char *location)
{
    if (!service || !trace) {
        return AIRY_ERR_INVALID_PARAM;
    }

    if (!service->config.enable_tracing) {
        AIRY_ERROR(AIRY_ERR_STATE_ERROR, "tracing is disabled");
    }

    trace->current_state = new_state;

    uint64_t now = get_timestamp_ms();
    switch (new_state) {
    case AGENT_STATE_CREATED:
        trace->start_time = now;
        break;
    case AGENT_STATE_RUNNING:
        if (!trace->last_update_time || trace->last_update_time < now - 1000)
            trace->last_update_time = now;
        break;
    case AGENT_STATE_WAITING:
    case AGENT_STATE_THINKING:
    case AGENT_STATE_EXECUTING_TOOL:
        if (location && trace->trace_point_count < trace->trace_point_capacity) {
            size_t idx = trace->trace_point_count++;
            agent_trace_point_t *tp = &trace->trace_points[idx];
            tp->timestamp = now;
            tp->state = new_state;
            AIRY_FREE(tp->location);
            tp->location = AIRY_STRDUP(location);
            tp->loop_count = 0;
            tp->memory_usage = 0;
            tp->cpu_usage = 0.0;
        }
        break;
    case AGENT_STATE_COMPLETED:
    case AGENT_STATE_FAILED:
    case AGENT_STATE_CANCELLED:
    case AGENT_STATE_STUCK:
        trace->last_update_time = now;
        break;
    default:
        break;
    }

    return AIRY_SUCCESS;
}

int monitor_service_check_loop(monitor_service_t *service, agent_execution_trace_t *trace,
                               bool *is_loop, double *confidence)
{
    if (!service || !trace || !is_loop || !confidence) {
        return AIRY_ERR_INVALID_PARAM;
    }

    *is_loop = false;
    *confidence = 0.0;

    if (trace->trace_point_count < 3) {
        return AIRY_SUCCESS;
    }

    size_t loop_count = 0;
    size_t total_pairs = trace->trace_point_count - 1;

    for (size_t i = 0; i < total_pairs; i++) {
        for (size_t j = i + 2; j < trace->trace_point_count && j < i + 5; j++) {
            agent_trace_point_t *pi = &trace->trace_points[i];
            agent_trace_point_t *pj = &trace->trace_points[j];
            if (pi->location && pj->location && strcmp(pi->location, pj->location) == 0) {
                loop_count++;
            }
        }
    }

    if (loop_count > 0) {
        double ratio = (double)loop_count / (double)total_pairs;
        if (ratio > (double)service->config.loop_threshold) {
            *is_loop = true;
            *confidence = ratio > 0.9 ? 0.95 : ratio;
            trace->is_suspected_loop = true;
            trace->loop_detection_count++;
        }
    }

    return AIRY_SUCCESS;
}

int monitor_service_end_agent_trace(monitor_service_t *service, agent_execution_trace_t *trace,
                                    agent_execution_state_t final_state __attribute__((unused)))
{
    if (!service || !trace) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->trace_lock);
    for (size_t i = 0; i < service->trace_count; i++) {
        if (service->traces[i].trace_id && trace->trace_id &&
            strcmp(service->traces[i].trace_id, trace->trace_id) == 0) {
            service->traces[i].end_time = get_timestamp_ms();
            service->traces[i].status = 0;
            break;
        }
    }
    airy_mtx_unlock(&service->trace_lock);

    AIRY_FREE(trace->agent_id);
    AIRY_FREE(trace->task_id);
    AIRY_FREE(trace->trace_id);
    AIRY_FREE(trace->service_name);

    if (trace->trace_points) {
        for (size_t i = 0; i < trace->trace_point_count; i++) {
            AIRY_FREE(trace->trace_points[i].location);
        }
        AIRY_FREE(trace->trace_points);
    }

    if (trace->locations) {
        for (size_t i = 0; i < trace->location_count; i++) {
            AIRY_FREE(trace->locations[i]);
        }
        AIRY_FREE(trace->locations);
    }
    AIRY_FREE(trace->location_times);

    AIRY_FREE(trace);
    return AIRY_SUCCESS;
}

int monitor_service_get_agent_summary(monitor_service_t *service, const char *agent_id,
                                      uint64_t start_time, uint64_t end_time, char **summary)
{
    if (!service || !summary) {
        return AIRY_ERR_INVALID_PARAM;
    }

    char buf[4096];
    snprintf(buf, sizeof(buf),
             "Agent summary for %s (time range: %llu - %llu)\n"
             "Traces: %zu, Alerts: %zu, Metrics: %zu\n",
             agent_id ? agent_id : "all", (unsigned long long)start_time,
             (unsigned long long)end_time, service->trace_count, service->alert_count,
             service->metric_cache_count);

    *summary = AIRY_STRDUP(buf);
    if (!*summary) {
        return AIRY_ENOMEM;
    }
    return AIRY_SUCCESS;
}

int monitor_service_export_agent_trace(monitor_service_t *service, agent_execution_trace_t *trace,
                                       const char *format, char **data, size_t *size)
{
    if (!service || !data || !size) {
        return AIRY_ERR_INVALID_PARAM;
    }

    const char *fmt = format ? format : "json";
    char buf[2048];

    if (strcmp(fmt, "json") == 0) {
        snprintf(buf, sizeof(buf), "{\"trace_id\":\"%s\",\"format\":\"json\"}",
                 trace && trace->agent_id ? trace->agent_id : "unknown");
    } else {
        snprintf(buf, sizeof(buf), "trace_id=%s\nformat=%s\n",
                 trace && trace->agent_id ? trace->agent_id : "unknown", fmt);
    }

    *data = AIRY_STRDUP(buf);
    *size = strlen(buf);
    return AIRY_SUCCESS;
}

int monitor_service_get_active_agents(monitor_service_t *service, char ***agent_ids, size_t *count)
{
    if (!service || !agent_ids || !count) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->trace_lock);

    size_t active = 0;
    for (size_t i = 0; i < service->trace_count; i++) {
        if (service->traces[i].end_time == 0) {
            active++;
        }
    }

    if (active == 0) {
        *agent_ids = NULL;
        *count = 0;
        airy_mtx_unlock(&service->trace_lock);
        return AIRY_SUCCESS;
    }

    char **ids = (char **)AIRY_CALLOC(active, sizeof(char *));
    if (!ids) {
        airy_mtx_unlock(&service->trace_lock);
        return AIRY_ENOMEM;
    }
    size_t idx = 0;
    for (size_t i = 0; i < service->trace_count && idx < active; i++) {
        if (service->traces[i].end_time == 0 && service->traces[i].service_name) {
            ids[idx++] = AIRY_STRDUP(service->traces[i].service_name);
        }
    }

    *agent_ids = ids;
    *count = active;

    airy_mtx_unlock(&service->trace_lock);
    return AIRY_SUCCESS;
}

int monitor_service_reset_loop_detection(monitor_service_t *service, agent_execution_trace_t *trace)
{
    if (!service || !trace) {
        return AIRY_ERR_INVALID_PARAM;
    }

    trace->is_suspected_loop = false;
    trace->loop_detection_count = 0;

    for (size_t i = 0; i < trace->trace_point_count; i++) {
        agent_trace_point_t *tp = &trace->trace_points[i];
        AIRY_FREE(tp->location);
        tp->location = NULL;
        tp->loop_count = 0;
    }
    trace->trace_point_count = 0;

    return AIRY_SUCCESS;
}
