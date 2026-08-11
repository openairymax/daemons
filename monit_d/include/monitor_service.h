/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file monitor_service.h
 * @brief 监控服务接口定义
 * @details 负责系统监控、指标收集、告警管理和日志记录
 */

#ifndef AIRY_RT_MONITOR_SERVICE_H
#define AIRY_RT_MONITOR_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 监控服务配置
 */
typedef struct {
    uint32_t metrics_collection_interval_ms;
    uint32_t health_check_interval_ms;
    uint32_t log_flush_interval_ms;
    uint32_t alert_check_interval_ms;
    char *log_file_path;
    char *metrics_storage_path;
    bool enable_tracing;
    bool enable_alerting;
    double loop_threshold;
} monitor_config_t;

/**
 * @brief 指标类型
 */
typedef enum {
    METRIC_TYPE_COUNTER,
    METRIC_TYPE_GAUGE, /**<  gauge */
    METRIC_TYPE_HISTOGRAM,
    METRIC_TYPE_SUMMARY,
    METRIC_TYPE_COUNT
} metric_type_t;

/**
 * @brief 告警级别
 */
typedef enum {
    ALERT_LEVEL_INFO,
    ALERT_LEVEL_WARNING,
    ALERT_LEVEL_ERROR,
    ALERT_LEVEL_CRITICAL,
    ALERT_LEVEL_COUNT
} alert_level_t;

/**
 * @brief 指标信息
 */
typedef struct {
    char *name;
    char *description;
    metric_type_t type;
    char **labels;
    size_t label_count;
    double value;
    uint64_t timestamp;
} metric_info_t;

/**
 * @brief 告警信息
 */
typedef struct {
    char *alert_id;
    char *message;
    alert_level_t level;
    char *service_name;
    char *resource_id;
    char **labels;
    size_t label_count;
    uint64_t timestamp;
    bool is_resolved;
} alert_info_t;

/**
 * @brief 日志级别
 */
#include <logging.h>
#ifndef LOG_LEVEL_WARNING
#define LOG_LEVEL_WARNING LOG_LEVEL_WARN
#endif

/**
 * @brief 日志信息
 */
typedef struct {
    log_level_t level;
    char *message;
    char *service_name;
    char *file;
    int line;
    char *function;
    uint64_t timestamp;
    char **context;
    size_t context_count;
} log_info_t;

/**
 * @brief 健康检查结果
 */
typedef struct {
    char *service_name;
    bool is_healthy;
    char *status_message;
    uint64_t timestamp;
    int error_code;
} health_check_result_t;

/**
 * @brief 监控服务句柄
 */
typedef struct monitor_service monitor_service_t;

/**
 * @brief 创建监控服务
 * @param manager 配置信息
 * @param service 输出参数，返回创建的服务句柄
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_create(const monitor_config_t *manager, monitor_service_t **service);

/**
 * @brief 销毁监控服务
 * @param service 服务句柄
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_destroy(monitor_service_t *service);

/**
 * @brief 记录指标
 * @param service 服务句柄
 * @param metric 指标信息
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_record_metric(monitor_service_t *service, const metric_info_t *metric);

/**
 * @brief 记录日志
 * @param service 服务句柄
 * @param log 日志信息
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_log(monitor_service_t *service, const log_info_t *log);

/**
 * @brief 触发告警
 * @param service 服务句柄
 * @param alert 告警信息
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_trigger_alert(monitor_service_t *service, const alert_info_t *alert);

/**
 * @brief 解决告警
 * @param service 服务句柄
 * @param alert_id 告警 ID
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_resolve_alert(monitor_service_t *service, const char *alert_id);

/**
 * @brief 执行健康检查
 * @param service 服务句柄
 * @param service_name 服务名称
 * @param result 输出参数，返回健康检查结果
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_health_check(monitor_service_t *service, const char *service_name,
                                 health_check_result_t **result);

/**
 * @brief 获取指标数据
 * @param service 服务句柄
 * @param metric_name 指标名称
 * @param metrics 输出参数，返回指标数据数组
 * @param count 输出参数，返回指标数量
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_get_metrics(monitor_service_t *service, const char *metric_name,
                                metric_info_t ***metrics, size_t *count);

/**
 * @brief 获取告警列表
 * @param service 服务句柄
 * @param alerts 输出参数，返回告警数组
 * @param count 输出参数，返回告警数量
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_get_alerts(monitor_service_t *service, alert_info_t ***alerts, size_t *count);

/**
 * @brief 重载配置
 * @param service 服务句柄
 * @param manager 新的配置信息
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_reload_config(monitor_service_t *service, const monitor_config_t *manager);

/**
 * @brief 生成监控报告
 * @param service 服务句柄
 * @param report 输出参数，返回报告内容
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_generate_report(monitor_service_t *service, char **report);


/**
 * @brief Agent 执行状态
 */
typedef enum {
    AGENT_STATE_CREATED = 0,
    AGENT_STATE_INITIALIZING,
    AGENT_STATE_READY,
    AGENT_STATE_RUNNING,
    AGENT_STATE_WAITING,
    AGENT_STATE_THINKING,
    AGENT_STATE_EXECUTING,
    AGENT_STATE_EXECUTING_TOOL,
    AGENT_STATE_PAUSED,
    AGENT_STATE_COMPLETED,
    AGENT_STATE_FAILED,
    AGENT_STATE_CANCELLED,
    AGENT_STATE_STUCK,
    AGENT_STATE_COUNT
} agent_execution_state_t;

/**
 * @brief 死循环检测模式
 */
typedef enum {
    LOOP_DETECTION_TIME_BASED = 0,
    LOOP_DETECTION_PATTERN_BASED,
    LOOP_DETECTION_RESOURCE_BASED,
    LOOP_DETECTION_HYBRID
} loop_detection_mode_t;

/**
 * @brief Agent 执行轨迹点
 */
typedef struct {
    uint64_t timestamp;
    agent_execution_state_t state;
    char *location;
    size_t loop_count;
    size_t memory_usage;
    double cpu_usage;
} agent_trace_point_t;

/**
 * @brief Agent 执行轨迹
 */
typedef struct {
    char *agent_id; /**< Agent ID */
    char *task_id;
    char *trace_id; /**< Trace ID */
    agent_execution_state_t current_state;
    uint64_t start_time;
    uint64_t end_time;
    uint64_t last_update_time;
    int status;
    char *service_name;
    agent_trace_point_t *trace_points;
    size_t trace_point_count;
    size_t trace_point_capacity;
    size_t loop_detection_count;
    bool is_suspected_loop;
    bool loop_detected;
    double loop_confidence;
    char **locations;
    uint64_t *location_times;
    size_t location_count;
} agent_execution_trace_t;

/**
 * @brief 死循环检测配置
 */
typedef struct {
    loop_detection_mode_t mode;
    uint64_t max_execution_time_ms;
    size_t max_loop_iterations;
    size_t pattern_window_size;
    double resource_threshold;
    bool enable_auto_recovery;
    bool enable_alerting;
} loop_detection_config_t;

/**
 * @brief 默认死循环检测配置
 */
#define LOOP_DETECTION_CONFIG_DEFAULT \
    {.mode = LOOP_DETECTION_HYBRID,   \
     .max_execution_time_ms = 30000,  \
     .max_loop_iterations = 1000,     \
     .pattern_window_size = 10,       \
     .resource_threshold = 0.9,       \
     .enable_auto_recovery = true,    \
     .enable_alerting = true}

/**
 * @brief 开始监控 Agent 执行
 *
 * @param service 监控服务句柄
 * @param agent_id Agent ID
 * @param task_id 任务 ID
 * @param loop_config 死循环检测配置（可为 NULL 使用默认配置）
 * @param trace 输出参数，返回执行轨迹句柄
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_start_agent_trace(monitor_service_t *service, const char *agent_id,
                                      const char *task_id,
                                      const loop_detection_config_t *loop_config,
                                      agent_execution_trace_t **trace);

/**
 * @brief 更新 Agent 执行状态
 *
 * @param service 监控服务句柄
 * @param trace 执行轨迹句柄
 * @param new_state 新状态
 * @param location 位置信息
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_update_agent_state(monitor_service_t *service, agent_execution_trace_t *trace,
                                       agent_execution_state_t new_state, const char *location);

/**
 * @brief 检查死循环
 *
 * @param service 监控服务句柄
 * @param trace 执行轨迹句柄
 * @param is_loop 输出参数，是否为死循环
 * @param confidence 输出参数，检测置信度（0.0-1.0）
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_check_loop(monitor_service_t *service, agent_execution_trace_t *trace,
                               bool *is_loop, double *confidence);

/**
 * @brief 结束 Agent 执行监控
 *
 * @param service 监控服务句柄
 * @param trace 执行轨迹句柄
 * @param final_state 最终状态
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_end_agent_trace(monitor_service_t *service, agent_execution_trace_t *trace,
                                    agent_execution_state_t final_state);

/**
 * @brief 获取 Agent 执行摘要
 *
 * @param service 监控服务句柄
 * @param agent_id Agent ID（可为 NULL 获取所有）
 * @param start_time 开始时间（可为 0）
 * @param end_time 结束时间（可为 0）
 * @param summary 输出参数，返回摘要信息
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_get_agent_summary(monitor_service_t *service, const char *agent_id,
                                      uint64_t start_time, uint64_t end_time, char **summary);

/**
 * @brief 导出 Agent 执行轨迹
 *
 * @param service 监控服务句柄
 * @param trace 执行轨迹句柄
 * @param format 导出格式（"json", "csv", "text"）
 * @param data 输出参数，返回导出数据
 * @param size 输出参数，返回数据大小
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_export_agent_trace(monitor_service_t *service, agent_execution_trace_t *trace,
                                       const char *format, char **data, size_t *size);

/**
 * @brief 获取活跃 Agent 列表
 *
 * @param service 监控服务句柄
 * @param agent_ids 输出参数，返回 Agent ID 数组
 * @param count 输出参数，返回 Agent 数量
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_get_active_agents(monitor_service_t *service, char ***agent_ids, size_t *count);

/**
 * @brief 重置死循环检测
 *
 * @param service 监控服务句柄
 * @param trace 执行轨迹句柄
 * @return 0 表示成功，非 0 表示错误码
 */
int monitor_service_reset_loop_detection(monitor_service_t *service,
                                         agent_execution_trace_t *trace);

#endif /* AIRY_RT_MONITOR_SERVICE_H */
