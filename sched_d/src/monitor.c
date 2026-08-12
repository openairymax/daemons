// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file monitor.c
 * @brief 监控模块实现
 * @details 监控 Agent 健康状态和系统运行状态
 */

#include "airy_memory.h"
#include "scheduler_service.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "error.h"

/**
 * @brief 监控数据
 */
typedef struct {
    uint64_t total_tasks;
    uint64_t successful_tasks;
    uint64_t failed_tasks;
    uint64_t total_execution_time_ms;
    time_t last_health_check;
    time_t last_stats_report;
    size_t available_agents;
    size_t total_agents;
} monitor_data_t;

/**
 * @brief 创建监控模块
 * @param data 输出参数，返回监控数据
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_create(void **data)
{
    monitor_data_t *md = (monitor_data_t *)AIRY_MALLOC(sizeof(monitor_data_t));
    if (!md) {
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    md->total_tasks = 0;
    md->successful_tasks = 0;
    md->failed_tasks = 0;
    md->total_execution_time_ms = 0;
    md->last_health_check = time(NULL);
    md->last_stats_report = time(NULL);
    md->available_agents = 0;
    md->total_agents = 0;

    *data = md;
    return 0;
}

/**
 * @brief Destroy the monitor module
 * @param data Monitor data
 * @return 0 on success, non-zero error code
 */
int monitor_destroy(void *data)
{
    if (!data) {
        return 0;
    }

    AIRY_FREE(data);
    return 0;
}

/**
 * @brief Record a task execution result
 * @param data              Monitor data
 * @param success           Whether it succeeded
 * @param execution_time_ms Execution time (ms)
 * @return 0 on success, non-zero error code
 */
int monitor_record_task(void *data, bool success, uint32_t execution_time_ms)
{
    if (!data) {
        return AIRY_ERR_INVALID_PARAM;
    }

    monitor_data_t *md = (monitor_data_t *)data;

    md->total_tasks++;
    if (success) {
        md->successful_tasks++;
    } else {
        md->failed_tasks++;
    }
    md->total_execution_time_ms += execution_time_ms;

    return 0;
}

/**
 * @brief Update agent status
 * @param data            Monitor data
 * @param available_count Number of available agents
 * @param total_count     Total number of agents
 * @return 0 on success, non-zero error code
 */
int monitor_update_agent_status(void *data, size_t available_count, size_t total_count)
{
    if (!data) {
        return AIRY_ERR_INVALID_PARAM;
    }

    monitor_data_t *md = (monitor_data_t *)data;
    md->available_agents = available_count;
    md->total_agents = total_count;

    return 0;
}

/**
 * @brief Run a health check
 * @param data          Monitor data
 * @param health_status Output param, returns the health status
 * @return 0 on success, non-zero error code
 */
int monitor_health_check(void *data, bool *health_status)
{
    if (!data || !health_status) {
        return AIRY_ERR_INVALID_PARAM;
    }

    monitor_data_t *md = (monitor_data_t *)data;

    bool healthy = true;

    if (md->total_agents > 0 && md->available_agents == 0) {
        healthy = false;
        SVC_LOG_WARN("Health check failed: No available agents");
    }

    if (md->total_tasks > 10) {
        float failure_rate = (float)md->failed_tasks / md->total_tasks;
        if (failure_rate > 0.5) {
            healthy = false;
            SVC_LOG_WARN("Health check failed: High failure rate (%.2f)", failure_rate);
        }
    }

    md->last_health_check = time(NULL);

    *health_status = healthy;
    return 0;
}

/**
 * @brief Get statistics
 * @param data  Monitor data
 * @param stats Output param, returns the statistics
 * @return 0 on success, non-zero error code
 */
int monitor_get_stats(void *data, void **stats)
{
    if (!data || !stats) {
        return AIRY_ERR_INVALID_PARAM;
    }

    monitor_data_t *md = (monitor_data_t *)data;

    char *stats_str = (char *)AIRY_MALLOC(512);
    if (!stats_str) {
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    float avg_execution_time = 0.0f;
    if (md->total_tasks > 0) {
        avg_execution_time = (float)md->total_execution_time_ms / md->total_tasks;
    }

    float success_rate = 0.0f;
    if (md->total_tasks > 0) {
        success_rate = (float)md->successful_tasks / md->total_tasks;
    }

    snprintf(stats_str, 512,
             "Total Tasks: %lu\n"
             "Successful Tasks: %lu\n"
             "Failed Tasks: %lu\n"
             "Success Rate: %.2f%%\n"
             "Average Execution Time: %.2f ms\n"
             "Available Agents: %zu/%zu\n"
             "Last Health Check: %s"
             "Last Stats Report: %s",
             (unsigned long)md->total_tasks, (unsigned long)md->successful_tasks,
             (unsigned long)md->failed_tasks, success_rate * 100.0f, avg_execution_time,
             md->available_agents, md->total_agents, ctime(&md->last_health_check),
             ctime(&md->last_stats_report));

    *stats = stats_str;
    return 0;
}

/**
 * @brief Generate a statistics report
 * @param data Monitor data
 * @return 0 on success, non-zero error code
 */
int monitor_generate_report(void *data)
{
    if (!data) {
        return AIRY_ERR_INVALID_PARAM;
    }

    monitor_data_t *md = (monitor_data_t *)data;

    void *stats = NULL;
    if (monitor_get_stats(data, &stats) == 0) {
        SVC_LOG_INFO("=== Scheduler Stats Report ===");
        SVC_LOG_INFO("%s", (char *)stats);
        SVC_LOG_INFO("=============================");
        AIRY_FREE(stats);
    }

    md->last_stats_report = time(NULL);

    return 0;
}
