/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file unified_metrics.h
 * @brief Unified metrics collector - aggregates all daemon metrics.
 *
 * Provides a single global metrics-collection entry, aggregating the
 * metrics of every daemon into one Prometheus endpoint, supporting:
 * - Global metric registry
 * - Export grouped by module/daemon
 * - Full Prometheus-format export
 * - Automatic metric annotation (module name, instance ID, etc.)
 *
 * @see metrics.h  basic metrics collection
 */

#ifndef AIRY_RT_UNIFIED_METRICS_H
#define AIRY_RT_UNIFIED_METRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#define UM_MAX_MODULES 32
#define UM_MAX_METRICS_PER_MOD 256
#define UM_MODULE_NAME_LEN 32
#define UM_METRIC_NAME_LEN 128


typedef enum {
    UM_TYPE_COUNTER = 0,
    UM_TYPE_GAUGE = 1,
    UM_TYPE_HISTOGRAM = 2,
    UM_TYPE_SUMMARY = 3
} um_metric_type_t;


typedef struct {
    char name[UM_METRIC_NAME_LEN];
    char help[256];
    um_metric_type_t type;
    char labels[256];
    double value;
    double sum;
    uint64_t count;
    uint64_t timestamp_ms;
} um_metric_entry_t;


typedef struct {
    char module_name[UM_MODULE_NAME_LEN];
    char instance_id[32];
    um_metric_entry_t metrics[UM_MAX_METRICS_PER_MOD];
    uint32_t metric_count;
    bool active;
} um_module_metrics_t;


typedef struct {
    char service_name[64];
    uint32_t scrape_interval_ms;
    uint32_t retention_ms;
    bool enable_default_metrics;
} um_config_t;


typedef struct {
    uint64_t total_registrations;
    uint64_t total_exports;
    uint64_t total_increments;
    uint64_t total_updates;
    uint32_t active_modules;
    uint32_t total_metrics;
} um_stats_t;


/**
 * @brief Initialize the unified metrics collector.
 * @param config Config params (NULL = defaults)
 * @return 0 on success, non-zero on failure
 */
int um_init(const um_config_t *config);

/** @brief Shut down the unified metrics collector. */
void um_shutdown(void);

/**
 * @brief Check whether initialized.
 * @return true if initialized, false otherwise
 */
bool um_is_initialized(void);


/**
 * @brief Register a metrics module.
 * @param module_name Module name (e.g. "gateway_d", "sched_d")
 * @param instance_id Instance ID (NULL = default)
 * @return 0 on success, non-zero on failure
 */
int um_register_module(const char *module_name, const char *instance_id);

/**
 * @brief Unregister a metrics module.
 * @param module_name Module name
 * @return 0 on success, non-zero on failure
 */
int um_unregister_module(const char *module_name);


/**
 * @brief Register a metric definition.
 * @param module_name Module name
 * @param name Metric name
 * @param type Metric type
 * @param help Help text
 * @param labels Labels (e.g. "method=\"GET\",path=\"/api\"")
 * @return 0 on success, non-zero on failure
 */
int um_register_metric(const char *module_name, const char *name, um_metric_type_t type,
                       const char *help, const char *labels);

/**
 * @brief Increment a counter.
 * @param module_name Module name
 * @param name Metric name
 * @param value Increment value
 * @return 0 on success, non-zero on failure
 */
int um_increment(const char *module_name, const char *name, uint64_t value);

/**
 * @brief Set a gauge value.
 * @param module_name Module name
 * @param name Metric name
 * @param value Value
 * @return 0 on success, non-zero on failure
 */
int um_gauge_set(const char *module_name, const char *name, double value);

/**
 * @brief Observe a histogram/summary value.
 * @param module_name Module name
 * @param name Metric name
 * @param value Observed value
 * @return 0 on success, non-zero on failure
 */
int um_observe(const char *module_name, const char *name, double value);


/**
 * @brief Export all metrics in Prometheus format.
 * @return Prometheus-format string (caller frees), NULL on failure
 */
char *um_export_prometheus(void);

/**
 * @brief Export one module's metrics in Prometheus format.
 * @param module_name Module name (NULL = export all)
 * @return Prometheus-format string (caller frees), NULL on failure
 */
char *um_export_prometheus_module(const char *module_name);

/**
 * @brief Export all metrics in JSON format.
 * @return JSON string (caller frees), NULL on failure
 */
char *um_export_json(void);


/**
 * @brief Register default system metrics (CPU/memory/threads, etc.).
 * @return 0 on success, non-zero on failure
 */
int um_register_default_metrics(void);

/** @brief Update the default system metrics. */
void um_update_default_metrics(void);


/**
 * @brief Get unified-metrics statistics.
 * @param stats [out] Statistics
 * @return 0 on success, non-zero on failure
 */
int um_get_stats(um_stats_t *stats);

/**
 * @brief Create a default config.
 * @return Default config
 */
um_config_t um_create_default_config(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_UNIFIED_METRICS_H */
