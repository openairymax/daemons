/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file monitor_service.h
 * @brief Monitoring-service interface definitions.
 * @details Handles system monitoring, metrics collection, alert management
 *          and log recording.
 */

#ifndef AIRY_RT_MONITOR_SERVICE_H
#define AIRY_RT_MONITOR_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Monitoring-service config. */
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

/** @brief Metric type. */
typedef enum {
    METRIC_TYPE_COUNTER,
    METRIC_TYPE_GAUGE, /**<  gauge */
    METRIC_TYPE_HISTOGRAM,
    METRIC_TYPE_SUMMARY,
    METRIC_TYPE_COUNT
} metric_type_t;

/** @brief Alert level. */
typedef enum {
    ALERT_LEVEL_INFO,
    ALERT_LEVEL_WARNING,
    ALERT_LEVEL_ERROR,
    ALERT_LEVEL_CRITICAL,
    ALERT_LEVEL_COUNT
} alert_level_t;

/** @brief Metric info. */
typedef struct {
    char *name;
    char *description;
    metric_type_t type;
    char **labels;
    size_t label_count;
    double value;
    uint64_t timestamp;
} metric_info_t;

/** @brief Alert info. */
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

/** @brief Log level. */
#include <logging.h>
#ifndef LOG_LEVEL_WARNING
#define LOG_LEVEL_WARNING LOG_LEVEL_WARN
#endif

/** @brief Log info. */
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

/** @brief Health-check result. */
typedef struct {
    char *service_name;
    bool is_healthy;
    char *status_message;
    uint64_t timestamp;
    int error_code;
} health_check_result_t;

/** @brief Monitoring-service handle. */
typedef struct monitor_service monitor_service_t;

/**
 * @brief Create a monitoring service.
 * @param manager Config info
 * @param service Output parameter, returns the created service handle
 * @return 0 on success, non-zero error code
 */
int monitor_service_create(const monitor_config_t *manager, monitor_service_t **service);

/**
 * @brief Destroy a monitoring service.
 * @param service Service handle
 * @return 0 on success, non-zero error code
 */
int monitor_service_destroy(monitor_service_t *service);

/**
 * @brief Record a metric.
 * @param service Service handle
 * @param metric Metric info
 * @return 0 on success, non-zero error code
 */
int monitor_service_record_metric(monitor_service_t *service, const metric_info_t *metric);

/**
 * @brief Record a log entry.
 * @param service Service handle
 * @param log Log info
 * @return 0 on success, non-zero error code
 */
int monitor_service_log(monitor_service_t *service, const log_info_t *log);

/**
 * @brief Trigger an alert.
 * @param service Service handle
 * @param alert Alert info
 * @return 0 on success, non-zero error code
 */
int monitor_service_trigger_alert(monitor_service_t *service, const alert_info_t *alert);

/**
 * @brief Resolve an alert.
 * @param service Service handle
 * @param alert_id Alert ID
 * @return 0 on success, non-zero error code
 */
int monitor_service_resolve_alert(monitor_service_t *service, const char *alert_id);

/**
 * @brief Run a health check.
 * @param service Service handle
 * @param service_name Service name
 * @param result Output parameter, returns the health-check result
 * @return 0 on success, non-zero error code
 */
int monitor_service_health_check(monitor_service_t *service, const char *service_name,
                                 health_check_result_t **result);

/**
 * @brief Get metric data.
 * @param service Service handle
 * @param metric_name Metric name
 * @param metrics Output parameter, returns the metric-data array
 * @param count Output parameter, returns the metric count
 * @return 0 on success, non-zero error code
 */
int monitor_service_get_metrics(monitor_service_t *service, const char *metric_name,
                                metric_info_t ***metrics, size_t *count);

/**
 * @brief Get the alert list.
 * @param service Service handle
 * @param alerts Output parameter, returns the alert array
 * @param count Output parameter, returns the alert count
 * @return 0 on success, non-zero error code
 */
int monitor_service_get_alerts(monitor_service_t *service, alert_info_t ***alerts, size_t *count);

/**
 * @brief Reload the config.
 * @param service Service handle
 * @param manager New config info
 * @return 0 on success, non-zero error code
 */
int monitor_service_reload_config(monitor_service_t *service, const monitor_config_t *manager);

/**
 * @brief Generate a monitoring report.
 * @param service Service handle
 * @param report Output parameter, returns the report content
 * @return 0 on success, non-zero error code
 */
int monitor_service_generate_report(monitor_service_t *service, char **report);


/** @brief Agent execution state. */
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

/** @brief Loop-detection mode. */
typedef enum {
    LOOP_DETECTION_TIME_BASED = 0,
    LOOP_DETECTION_PATTERN_BASED,
    LOOP_DETECTION_RESOURCE_BASED,
    LOOP_DETECTION_HYBRID
} loop_detection_mode_t;

/** @brief Agent execution trace point. */
typedef struct {
    uint64_t timestamp;
    agent_execution_state_t state;
    char *location;
    size_t loop_count;
    size_t memory_usage;
    double cpu_usage;
} agent_trace_point_t;

/** @brief Agent execution trace. */
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

/** @brief Loop-detection config. */
typedef struct {
    loop_detection_mode_t mode;
    uint64_t max_execution_time_ms;
    size_t max_loop_iterations;
    size_t pattern_window_size;
    double resource_threshold;
    bool enable_auto_recovery;
    bool enable_alerting;
} loop_detection_config_t;

/** @brief Default loop-detection config. */
#define LOOP_DETECTION_CONFIG_DEFAULT \
    {.mode = LOOP_DETECTION_HYBRID,   \
     .max_execution_time_ms = 30000,  \
     .max_loop_iterations = 1000,     \
     .pattern_window_size = 10,       \
     .resource_threshold = 0.9,       \
     .enable_auto_recovery = true,    \
     .enable_alerting = true}

/**
 * @brief Start monitoring an agent execution.
 *
 * @param service Monitoring-service handle
 * @param agent_id Agent ID
 * @param task_id Task ID
 * @param loop_config Loop-detection config (NULL = default config)
 * @param trace Output parameter, returns the execution-trace handle
 * @return 0 on success, non-zero error code
 */
int monitor_service_start_agent_trace(monitor_service_t *service, const char *agent_id,
                                      const char *task_id,
                                      const loop_detection_config_t *loop_config,
                                      agent_execution_trace_t **trace);

/**
 * @brief Update the agent execution state.
 *
 * @param service Monitoring-service handle
 * @param trace Execution-trace handle
 * @param new_state New state
 * @param location Location info
 * @return 0 on success, non-zero error code
 */
int monitor_service_update_agent_state(monitor_service_t *service, agent_execution_trace_t *trace,
                                       agent_execution_state_t new_state, const char *location);

/**
 * @brief Check for a loop.
 *
 * @param service Monitoring-service handle
 * @param trace Execution-trace handle
 * @param is_loop Output parameter, whether it is a loop
 * @param confidence Output parameter, detection confidence (0.0-1.0)
 * @return 0 on success, non-zero error code
 */
int monitor_service_check_loop(monitor_service_t *service, agent_execution_trace_t *trace,
                               bool *is_loop, double *confidence);

/**
 * @brief End agent-execution monitoring.
 *
 * @param service Monitoring-service handle
 * @param trace Execution-trace handle
 * @param final_state Final state
 * @return 0 on success, non-zero error code
 */
int monitor_service_end_agent_trace(monitor_service_t *service, agent_execution_trace_t *trace,
                                    agent_execution_state_t final_state);

/**
 * @brief Get the agent-execution summary.
 *
 * @param service Monitoring-service handle
 * @param agent_id Agent ID (NULL = all)
 * @param start_time Start time (may be 0)
 * @param end_time End time (may be 0)
 * @param summary Output parameter, returns the summary
 * @return 0 on success, non-zero error code
 */
int monitor_service_get_agent_summary(monitor_service_t *service, const char *agent_id,
                                      uint64_t start_time, uint64_t end_time, char **summary);

/**
 * @brief Export an agent-execution trace.
 *
 * @param service Monitoring-service handle
 * @param trace Execution-trace handle
 * @param format Export format ("json", "csv", "text")
 * @param data Output parameter, returns the exported data
 * @param size Output parameter, returns the data size
 * @return 0 on success, non-zero error code
 */
int monitor_service_export_agent_trace(monitor_service_t *service, agent_execution_trace_t *trace,
                                       const char *format, char **data, size_t *size);

/**
 * @brief Get the active-agent list.
 *
 * @param service Monitoring-service handle
 * @param agent_ids Output parameter, returns the Agent-ID array
 * @param count Output parameter, returns the agent count
 * @return 0 on success, non-zero error code
 */
int monitor_service_get_active_agents(monitor_service_t *service, char ***agent_ids, size_t *count);

/**
 * @brief Reset loop detection.
 *
 * @param service Monitoring-service handle
 * @param trace Execution-trace handle
 * @return 0 on success, non-zero error code
 */
int monitor_service_reset_loop_detection(monitor_service_t *service,
                                         agent_execution_trace_t *trace);

#endif /* AIRY_RT_MONITOR_SERVICE_H */
