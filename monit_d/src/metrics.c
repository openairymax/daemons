// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"

/**
 * @file metrics.c
 * @brief Monitoring metrics-collection implementation (production-grade).
 *
 * Features:
 * 1. Metrics collection and aggregation
 * 2. OpenTelemetry compatibility
 * 3. Metrics export (Prometheus format)
 * 4. Thread safety
 */

#include "daemon_errors.h"
#include "monitor_service.h"
#include "daemon_platform_ext.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_METRICS 1024
#define MAX_LABELS_PER_METRIC 16
#define MAX_SERIES_PER_METRIC 256
#define MAX_LABEL_VALUE_LEN 256
#define DEFAULT_EXPORT_INTERVAL_MS 60000
#define DEFAULT_RETENTION_SECONDS 3600

typedef struct {
    uint32_t export_interval_ms;
    uint32_t retention_seconds;
} metrics_config_t;

typedef struct {
    char *key;
    char *value;
} metric_label_t;

typedef struct {
    metric_label_t labels[MAX_LABELS_PER_METRIC];
    size_t label_count;
    double value;
    uint64_t timestamp;
    uint64_t update_count;
} metric_series_t;

typedef struct {
    double boundary;
    uint64_t count;
} histogram_bucket_t;

typedef struct {
    histogram_bucket_t *buckets;
    size_t bucket_count;
    double sum;
    uint64_t count;
} histogram_data_t;

typedef struct {
    char *name;
    char *description;
    char *unit;
    metric_type_t type;
    metric_series_t series[MAX_SERIES_PER_METRIC];
    size_t series_count;
    histogram_data_t histogram;
    airy_mtx_t lock;
    int initialized;
} metric_t;

typedef struct {
    metric_t metrics[MAX_METRICS];
    size_t metric_count;
    airy_mtx_t global_lock;
    uint64_t export_interval_ms;
    uint64_t retention_seconds;
    int initialized;
} metrics_storage_t;

static metrics_storage_t g_metrics = {0};

/**
 * @brief Find the metric index
 */
static int find_metric_index(const char *name)
{
    for (size_t i = 0; i < g_metrics.metric_count; i++) {
        if (strcmp(g_metrics.metrics[i].name, name) == 0) {
            return (int)i;
        }
    }
    return AIRY_ERR_NOT_FOUND;
}

/**
 * @brief Find the time-series index
 */
static int find_series_index(metric_t *metric, const metric_label_t *labels, size_t label_count)
{
    for (size_t i = 0; i < metric->series_count; i++) {
        metric_series_t *series = &metric->series[i];

        if (series->label_count != label_count) {
            continue;
        }

        int match = 1;
        for (size_t j = 0; j < label_count && match; j++) {
            int found = 0;
            for (size_t k = 0; k < series->label_count; k++) {
                if (strcmp(series->labels[k].key, labels[j].key) == 0 &&
                    strcmp(series->labels[k].value, labels[j].value) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found)
                match = 0;
        }

        if (match)
            return (int)i;
    }
    return AIRY_ERR_NOT_FOUND;
}

/**
 * @brief Create a time series
 */
static int create_series(metric_t *metric, const metric_label_t *labels, size_t label_count)
{
    if (metric->series_count >= MAX_SERIES_PER_METRIC) {
        return AIRY_ERR_OVERFLOW;
    }

    metric_series_t *series = &metric->series[metric->series_count];
    series->label_count = label_count;

    for (size_t i = 0; i < label_count; i++) {
        series->labels[i].key = AIRY_STRDUP(labels[i].key);
        if (!series->labels[i].key) {

            for (size_t k = 0; k < i; k++) {
                AIRY_FREE(series->labels[k].key);
                AIRY_FREE(series->labels[k].value);
            }
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        series->labels[i].value = AIRY_STRDUP(labels[i].value);
        if (!series->labels[i].value) {
            AIRY_FREE(series->labels[i].key);
            for (size_t k = 0; k < i; k++) {
                AIRY_FREE(series->labels[k].key);
                AIRY_FREE(series->labels[k].value);
            }
            return AIRY_ERR_OUT_OF_MEMORY;
        }
    }

    series->value = 0.0;
    series->timestamp = airy_time_ms();
    series->update_count = 0;

    metric->series_count++;
    return (int)(metric->series_count - 1);
}

/**
 * @brief Format labels in Prometheus format
 */
static void format_labels(char *buf, size_t buf_size, const metric_label_t *labels,
                          size_t label_count)
{
    if (label_count == 0) {
        buf[0] = '\0';
        return;
    }

    size_t pos = 0;
    buf[pos++] = '{';

    for (size_t i = 0; i < label_count && pos < buf_size - 2; i++) {
        if (i > 0) {
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }

        /* P1: clamp against would-be length so pos never overflows the
         * buffer and later writes cannot underflow their size arg. */
        size_t room = buf_size - pos;
        int written = snprintf(buf + pos, room, "%s=\"%s\"", labels[i].key, labels[i].value);
        if (written < 0) {
            break;
        }
        pos += ((size_t)written < room) ? (size_t)written : room - 1;
    }

    buf[pos++] = '}';
    buf[pos] = '\0';
}

/**
 * @brief Initialize the metrics system
 */
int metrics_init(const metrics_config_t *manager)
{
    if (g_metrics.initialized) {
        return AIRY_OK;
    }

    airy_mtx_init(&g_metrics.global_lock);

    g_metrics.metric_count = 0;
    g_metrics.export_interval_ms =
        manager ? manager->export_interval_ms : DEFAULT_EXPORT_INTERVAL_MS;
    g_metrics.retention_seconds = manager ? manager->retention_seconds : DEFAULT_RETENTION_SECONDS;

    for (size_t i = 0; i < MAX_METRICS; i++) {
        airy_mtx_init(&g_metrics.metrics[i].lock);
    }

    g_metrics.initialized = 1;
    return AIRY_OK;
}

/**
 * @brief Shut down the metrics system
 */
void metrics_shutdown(void)
{
    if (!g_metrics.initialized) {
        return;
    }

    airy_mtx_lock(&g_metrics.global_lock);

    for (size_t i = 0; i < g_metrics.metric_count; i++) {
        metric_t *metric = &g_metrics.metrics[i];

        airy_mtx_lock(&metric->lock);

        AIRY_FREE(metric->name);
        AIRY_FREE(metric->description);
        AIRY_FREE(metric->unit);

        for (size_t j = 0; j < metric->series_count; j++) {
            metric_series_t *series = &metric->series[j];
            for (size_t k = 0; k < series->label_count; k++) {
                AIRY_FREE(series->labels[k].key);
                AIRY_FREE(series->labels[k].value);
            }
        }

        if (metric->histogram.buckets) {
            AIRY_FREE(metric->histogram.buckets);
        }

        airy_mtx_unlock(&metric->lock);
        airy_mtx_destroy(&metric->lock);
    }

    g_metrics.metric_count = 0;
    g_metrics.initialized = 0;

    airy_mtx_unlock(&g_metrics.global_lock);
    airy_mtx_destroy(&g_metrics.global_lock);
}

/**
 * @brief Register a metric
 */
int metrics_register(const char *name, const char *description, const char *unit,
                     metric_type_t type, const double *histogram_buckets, size_t bucket_count)
{
    if (!name) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "name is NULL");
    }

    if (!g_metrics.initialized) {
        AIRY_ERROR(AIRY_ERR_STATE_ERROR, "Metrics system not initialized");
    }

    airy_mtx_lock(&g_metrics.global_lock);

    if (find_metric_index(name) >= 0) {
        airy_mtx_unlock(&g_metrics.global_lock);
        AIRY_ERROR(AIRY_ERR_ALREADY_EXISTS, "Metric already exists");
    }

    if (g_metrics.metric_count >= MAX_METRICS) {
        airy_mtx_unlock(&g_metrics.global_lock);
        AIRY_ERROR(AIRY_ERR_OVERFLOW, "Too many metrics");
    }

    metric_t *metric = &g_metrics.metrics[g_metrics.metric_count];
    metric->name = AIRY_STRDUP(name);
    if (!metric->name) {
        airy_mtx_unlock(&g_metrics.global_lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "Failed to duplicate metric name");
    }
    metric->description = description ? AIRY_STRDUP(description) : NULL;
    if (description && !metric->description) {
        AIRY_FREE(metric->name);
        airy_mtx_unlock(&g_metrics.global_lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "Failed to duplicate metric description");
    }
    metric->unit = unit ? AIRY_STRDUP(unit) : NULL;
    if (unit && !metric->unit) {
        AIRY_FREE(metric->description);
        AIRY_FREE(metric->name);
        airy_mtx_unlock(&g_metrics.global_lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "Failed to duplicate metric unit");
    }
    metric->type = type;
    metric->series_count = 0;
    metric->initialized = 1;

    if (type == METRIC_TYPE_HISTOGRAM && histogram_buckets && bucket_count > 0) {
        metric->histogram.buckets =
            (histogram_bucket_t *)AIRY_MALLOC(sizeof(histogram_bucket_t) * bucket_count);
        if (metric->histogram.buckets) {
            for (size_t i = 0; i < bucket_count; i++) {
                metric->histogram.buckets[i].boundary = histogram_buckets[i];
                metric->histogram.buckets[i].count = 0;
            }
            metric->histogram.bucket_count = bucket_count;
            metric->histogram.sum = 0.0;
            metric->histogram.count = 0;
        }
    }

    g_metrics.metric_count++;

    airy_mtx_unlock(&g_metrics.global_lock);
    return AIRY_OK;
}

/**
 * @brief Record a Counter
 */
int metrics_counter_inc(const char *name, const metric_label_t *labels, size_t label_count)
{
    if (!name || !g_metrics.initialized) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&g_metrics.global_lock);

    int idx = find_metric_index(name);
    if (idx < 0) {
        airy_mtx_unlock(&g_metrics.global_lock);
        return AIRY_ERR_NOT_FOUND;
    }

    metric_t *metric = &g_metrics.metrics[idx];
    airy_mtx_lock(&metric->lock);
    airy_mtx_unlock(&g_metrics.global_lock);

    int series_idx = find_series_index(metric, labels, label_count);
    if (series_idx < 0) {
        series_idx = create_series(metric, labels, label_count);
    }

    if (series_idx >= 0) {
        metric->series[series_idx].value += 1.0;
        metric->series[series_idx].timestamp = airy_time_ms();
        metric->series[series_idx].update_count++;
    }

    airy_mtx_unlock(&metric->lock);
    return AIRY_OK;
}

/**
 * @brief Record a Counter (increment by value)
 */
int metrics_counter_add(const char *name, double value, const metric_label_t *labels,
                        size_t label_count)
{
    if (!name || value < 0 || !g_metrics.initialized) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&g_metrics.global_lock);

    int idx = find_metric_index(name);
    if (idx < 0) {
        airy_mtx_unlock(&g_metrics.global_lock);
        return AIRY_ERR_NOT_FOUND;
    }

    metric_t *metric = &g_metrics.metrics[idx];
    airy_mtx_lock(&metric->lock);
    airy_mtx_unlock(&g_metrics.global_lock);

    int series_idx = find_series_index(metric, labels, label_count);
    if (series_idx < 0) {
        series_idx = create_series(metric, labels, label_count);
    }

    if (series_idx >= 0) {
        metric->series[series_idx].value += value;
        metric->series[series_idx].timestamp = airy_time_ms();
        metric->series[series_idx].update_count++;
    }

    airy_mtx_unlock(&metric->lock);
    return AIRY_OK;
}

/**
 * @brief Set a Gauge
 */
int metrics_gauge_set(const char *name, double value, const metric_label_t *labels,
                      size_t label_count)
{
    if (!name || !g_metrics.initialized) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&g_metrics.global_lock);

    int idx = find_metric_index(name);
    if (idx < 0) {
        airy_mtx_unlock(&g_metrics.global_lock);
        return AIRY_ERR_NOT_FOUND;
    }

    metric_t *metric = &g_metrics.metrics[idx];
    airy_mtx_lock(&metric->lock);
    airy_mtx_unlock(&g_metrics.global_lock);

    int series_idx = find_series_index(metric, labels, label_count);
    if (series_idx < 0) {
        series_idx = create_series(metric, labels, label_count);
    }

    if (series_idx >= 0) {
        metric->series[series_idx].value = value;
        metric->series[series_idx].timestamp = airy_time_ms();
        metric->series[series_idx].update_count++;
    }

    airy_mtx_unlock(&metric->lock);
    return AIRY_OK;
}

/**
 * @brief Record a Histogram
 */
int metrics_histogram_observe(const char *name, double value, const metric_label_t *labels,
                              size_t label_count)
{
    if (!name || !g_metrics.initialized) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&g_metrics.global_lock);

    int idx = find_metric_index(name);
    if (idx < 0) {
        airy_mtx_unlock(&g_metrics.global_lock);
        return AIRY_ERR_NOT_FOUND;
    }

    metric_t *metric = &g_metrics.metrics[idx];
    airy_mtx_lock(&metric->lock);
    airy_mtx_unlock(&g_metrics.global_lock);

    for (size_t i = 0; i < metric->histogram.bucket_count; i++) {
        if (value <= metric->histogram.buckets[i].boundary) {
            metric->histogram.buckets[i].count++;
        }
    }

    metric->histogram.sum += value;
    metric->histogram.count++;

    int series_idx = find_series_index(metric, labels, label_count);
    if (series_idx < 0) {
        series_idx = create_series(metric, labels, label_count);
    }

    if (series_idx >= 0) {
        metric->series[series_idx].value = value;
        metric->series[series_idx].timestamp = airy_time_ms();
        metric->series[series_idx].update_count++;
    }

    airy_mtx_unlock(&metric->lock);
    return AIRY_OK;
}

/**
 * @brief Export in Prometheus format
 */
char *metrics_export_prometheus(void)
{
    if (!g_metrics.initialized) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    size_t buf_size = 64 * 1024; /* 64KB */
    char *buf = (char *)AIRY_MALLOC(buf_size);
    if (!buf) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null buffer");
    }

    size_t pos = 0;

    airy_mtx_lock(&g_metrics.global_lock);

    /* P1: snprintf returns the would-be length; blind accumulation lets pos
     * overflow buf_size and underflow later size args. Append with clamping. */
#define PROM_APPEND(...)                                                  \
    do {                                                                  \
        if (pos >= buf_size)                                              \
            break;                                                        \
        size_t _room = buf_size - pos;                                    \
        int _w = snprintf(buf + pos, _room, __VA_ARGS__);                 \
        if (_w < 0)                                                       \
            break;                                                        \
        pos += ((size_t)_w < _room) ? (size_t)_w : _room - 1;             \
    } while (0)

    for (size_t i = 0; i < g_metrics.metric_count; i++) {
        metric_t *metric = &g_metrics.metrics[i];
        airy_mtx_lock(&metric->lock);

        if (metric->description) {
            PROM_APPEND("# HELP %s %s\n", metric->name, metric->description);
        }

        const char *type_str = "untyped";
        switch (metric->type) {
        case METRIC_TYPE_COUNTER:
            type_str = "counter";
            break;
        case METRIC_TYPE_GAUGE:
            type_str = "gauge";
            break;
        case METRIC_TYPE_HISTOGRAM:
            type_str = "histogram";
            break;
        case METRIC_TYPE_SUMMARY:
            type_str = "summary";
            break;
        case METRIC_TYPE_COUNT:
            type_str = "untyped";
            break;
        }
        PROM_APPEND("# TYPE %s %s\n", metric->name, type_str);

        for (size_t j = 0; j < metric->series_count; j++) {
            metric_series_t *series = &metric->series[j];

            char labels_buf[1024];
            format_labels(labels_buf, sizeof(labels_buf), series->labels, series->label_count);

            PROM_APPEND("%s%s %.17g %llu\n", metric->name, labels_buf, series->value,
                        (unsigned long long)series->timestamp);
        }

        if (metric->type == METRIC_TYPE_HISTOGRAM && metric->histogram.bucket_count > 0) {
            for (size_t b = 0; b < metric->histogram.bucket_count; b++) {
                PROM_APPEND("%s_bucket{le=\"%.17g\"} %llu\n", metric->name,
                            metric->histogram.buckets[b].boundary,
                            (unsigned long long)metric->histogram.buckets[b].count);
            }
            PROM_APPEND("%s_bucket{le=\"+Inf\"} %llu\n", metric->name,
                        (unsigned long long)metric->histogram.count);
            PROM_APPEND("%s_sum %.17g\n", metric->name, metric->histogram.sum);
            PROM_APPEND("%s_count %llu\n", metric->name,
                        (unsigned long long)metric->histogram.count);
        }

        PROM_APPEND("\n");

        airy_mtx_unlock(&metric->lock);
    }
#undef PROM_APPEND

    airy_mtx_unlock(&g_metrics.global_lock);

    return buf;
}

/**
 * @brief Get a metric value
 */
int metrics_get_value(const char *name, const metric_label_t *labels, size_t label_count,
                      double *value)
{
    if (!name || !value || !g_metrics.initialized) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&g_metrics.global_lock);

    int idx = find_metric_index(name);
    if (idx < 0) {
        airy_mtx_unlock(&g_metrics.global_lock);
        return AIRY_ERR_NOT_FOUND;
    }

    metric_t *metric = &g_metrics.metrics[idx];
    airy_mtx_lock(&metric->lock);
    airy_mtx_unlock(&g_metrics.global_lock);

    int series_idx = find_series_index(metric, labels, label_count);
    if (series_idx < 0) {
        airy_mtx_unlock(&metric->lock);
        return AIRY_ERR_NOT_FOUND;
    }

    *value = metric->series[series_idx].value;

    airy_mtx_unlock(&metric->lock);
    return AIRY_OK;
}

/**
 * @brief Get the total metric count
 */
size_t metrics_get_count(void)
{
    return g_metrics.metric_count;
}

/**
 * @brief Get the time-series count
 */
size_t metrics_get_series_count(const char *name)
{
    if (!name || !g_metrics.initialized) {
        return 0;
    }

    airy_mtx_lock(&g_metrics.global_lock);

    int idx = find_metric_index(name);
    if (idx < 0) {
        airy_mtx_unlock(&g_metrics.global_lock);
        return 0;
    }

    metric_t *metric = &g_metrics.metrics[idx];
    airy_mtx_lock(&metric->lock);
    airy_mtx_unlock(&g_metrics.global_lock);

    size_t count = metric->series_count;

    airy_mtx_unlock(&metric->lock);
    return count;
}
