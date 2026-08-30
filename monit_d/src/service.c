// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file service.c
 * @brief Monitor service 生命周期与配置域：monitor_service_create /
 *        monitor_service_destroy / monitor_service_reload_config，以及
 *        时间戳工具 get_timestamp_ms()。
 *
 * 2026-08-27 域拆分（原 900 行 → 3 文件）：指标/日志/告警/健康/报告域见
 * service_subsystems.c，Agent 执行追踪域见 service_agent_trace.c；
 * struct monitor_service 与各子系统条目类型经 monitor_service_internal.h
 * 共享。
 */

#include "airy_memory.h"
#include "daemon_errors.h"
#include "monitor_service.h"
#include "daemon_platform_ext.h"
#include "monitor_service_internal.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

uint64_t get_timestamp_ms(void)
{
    /* 校正后逻辑墙钟：联网以时区标准时间为准，离线回退系统时间，
     * 单调递增不受系统时间跳变影响（任务1，0.1.6f）。 */
    return airy_time_wall_ms();
}

int monitor_service_create(const monitor_config_t *config, monitor_service_t **service)
{
    if (!service) {
        return AIRY_ERR_INVALID_PARAM;
    }

    monitor_service_t *svc = (monitor_service_t *)AIRY_CALLOC(1, sizeof(monitor_service_t));
    if (!svc) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate monitor service");
    }

    if (config) {
        __builtin_memcpy(&svc->config, config, sizeof(monitor_config_t));
        if (config->log_file_path) {
            svc->config.log_file_path = AIRY_STRDUP(config->log_file_path);
            if (!svc->config.log_file_path) {
                AIRY_FREE(svc);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate log_file_path");
            }
        }
        if (config->metrics_storage_path) {
            svc->config.metrics_storage_path = AIRY_STRDUP(config->metrics_storage_path);
            if (!svc->config.metrics_storage_path) {
                AIRY_FREE(svc->config.log_file_path);
                AIRY_FREE(svc);
                AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate metrics_storage_path");
            }
        }
    } else {
        svc->config.metrics_collection_interval_ms = 5000;
        svc->config.health_check_interval_ms = 10000;
        svc->config.log_flush_interval_ms = 30000;
        svc->config.alert_check_interval_ms = 5000;
        svc->config.log_file_path = AIRY_STRDUP("monitor.log");
        if (!svc->config.log_file_path) {
            AIRY_FREE(svc);
            AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate default log_file_path");
        }
        svc->config.metrics_storage_path = AIRY_STRDUP("metrics");
        if (!svc->config.metrics_storage_path) {
            AIRY_FREE(svc->config.log_file_path);
            AIRY_FREE(svc);
            AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate default metrics_storage_path");
        }
        svc->config.enable_tracing = true;
        svc->config.enable_alerting = true;
    }

    airy_mtx_init(&svc->alert_lock);
    airy_mtx_init(&svc->log_lock);
    airy_mtx_init(&svc->trace_lock);
    airy_mtx_init(&svc->metric_lock);

    svc->alert_count = 0;
    svc->log_count = 0;
    svc->log_write_idx = 0;
    svc->trace_count = 0;
    svc->metric_cache_count = 0;
    svc->initialized = 1;
    svc->running = 0;

    *service = svc;

    SVC_LOG_INFO("Monitor service created (metrics_interval=%ums, tracing=%s, alerting=%s)",
                 svc->config.metrics_collection_interval_ms,
                 svc->config.enable_tracing ? "on" : "off",
                 svc->config.enable_alerting ? "on" : "off");
    return AIRY_SUCCESS;
}

int monitor_service_destroy(monitor_service_t *service)
{
    if (!service) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->alert_lock);
    for (size_t i = 0; i < service->alert_count; i++) {
        AIRY_FREE(service->alerts[i].alert_id);
        AIRY_FREE(service->alerts[i].message);
        AIRY_FREE(service->alerts[i].service_name);
        AIRY_FREE(service->alerts[i].resource_id);
    }
    airy_mtx_unlock(&service->alert_lock);
    airy_mtx_destroy(&service->alert_lock);

    airy_mtx_lock(&service->log_lock);
    for (size_t i = 0; i < service->log_count; i++) {
        size_t idx = (service->log_write_idx - service->log_count + i) % MAX_LOG_ENTRIES;
        AIRY_FREE(service->logs[idx].message);
        AIRY_FREE(service->logs[idx].service_name);
        AIRY_FREE(service->logs[idx].file);
        AIRY_FREE(service->logs[idx].function);
    }
    airy_mtx_unlock(&service->log_lock);
    airy_mtx_destroy(&service->log_lock);

    airy_mtx_lock(&service->trace_lock);
    for (size_t i = 0; i < service->trace_count; i++) {
        AIRY_FREE(service->traces[i].trace_id);
        AIRY_FREE(service->traces[i].operation_name);
        AIRY_FREE(service->traces[i].service_name);
    }
    airy_mtx_unlock(&service->trace_lock);
    airy_mtx_destroy(&service->trace_lock);

    airy_mtx_lock(&service->metric_lock);
    for (size_t i = 0; i < service->metric_cache_count; i++) {
        if (service->metric_cache[i]) {
            AIRY_FREE(service->metric_cache[i]->name);
            AIRY_FREE(service->metric_cache[i]->description);
            AIRY_FREE(service->metric_cache[i]);
        }
    }
    airy_mtx_unlock(&service->metric_lock);
    airy_mtx_destroy(&service->metric_lock);

    AIRY_FREE(service->config.log_file_path);
    AIRY_FREE(service->config.metrics_storage_path);

    service->initialized = 0;
    AIRY_FREE(service);

    SVC_LOG_INFO("Monitor service destroyed");
    return AIRY_SUCCESS;
}

int monitor_service_reload_config(monitor_service_t *service, const monitor_config_t *config)
{
    if (!service || !config) {
        return AIRY_ERR_INVALID_PARAM;
    }

    AIRY_FREE(service->config.log_file_path);
    service->config.log_file_path = NULL;
    AIRY_FREE(service->config.metrics_storage_path);
    service->config.metrics_storage_path = NULL;

    __builtin_memcpy(&service->config, config, sizeof(monitor_config_t));
    service->config.log_file_path =
        config->log_file_path ? AIRY_STRDUP(config->log_file_path) : NULL;
    service->config.metrics_storage_path =
        config->metrics_storage_path ? AIRY_STRDUP(config->metrics_storage_path) : NULL;

    SVC_LOG_INFO("Monitor service config reloaded");
    return AIRY_SUCCESS;
}
