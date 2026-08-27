// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file monitor_service_internal.h
 * @brief Monitor 服务拆分文件间的共享内部类型与声明（2026-08-27）。
 *
 * service.c 按单一职责拆分为三个文件：
 *   - service.c              生命周期与配置域（create/destroy/reload_config）
 *   - service_subsystems.c   指标/日志/告警/健康/报告域
 *   - service_agent_trace.c  Agent 执行追踪域（loop 检测等）
 * 内部结构体（struct monitor_service）与各子系统条目类型经此头共享。
 */

#ifndef AIRY_RT_MONITOR_SERVICE_INTERNAL_H
#define AIRY_RT_MONITOR_SERVICE_INTERNAL_H

#include "monitor_service.h"

#include "daemon_platform_ext.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_ALERTS 1024
#define MAX_LOG_ENTRIES 4096
#define MAX_TRACES 512
#define MAX_TRACE_SPANS 64
#define MAX_REPORT_SIZE 65536
#define MAX_METRICS 256

typedef struct {
    char *alert_id;
    char *message;
    alert_level_t level;
    char *service_name;
    char *resource_id;
    uint64_t timestamp;
    bool is_resolved;
} alert_entry_t;

typedef struct {
    log_level_t level;
    char *message;
    char *service_name;
    char *file;
    int line;
    char *function;
    uint64_t timestamp;
} log_entry_t;

typedef struct {
    char *trace_id;
    char *operation_name;
    uint64_t start_time;
    uint64_t end_time;
    int status;
    char *service_name;
    size_t span_count;
} trace_entry_t;

struct monitor_service {
    monitor_config_t config;

    alert_entry_t alerts[MAX_ALERTS];
    size_t alert_count;
    airy_mtx_t alert_lock;

    log_entry_t logs[MAX_LOG_ENTRIES];
    size_t log_count;
    size_t log_write_idx;
    airy_mtx_t log_lock;

    trace_entry_t traces[MAX_TRACES];
    size_t trace_count;
    airy_mtx_t trace_lock;

    metric_info_t *metric_cache[MAX_METRICS];
    size_t metric_cache_count;
    airy_mtx_t metric_lock;

    int initialized;
    int running;
};

/* 时间戳工具（service.c 定义，各域共用） */
uint64_t get_timestamp_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_MONITOR_SERVICE_INTERNAL_H */
