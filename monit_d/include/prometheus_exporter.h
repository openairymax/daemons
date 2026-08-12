/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file prometheus_exporter.h
 * @brief C-L10: Prometheus scrape endpoint for monit_d.
 *
 * Provides a /metrics HTTP endpoint exposing Prometheus-format metrics,
 * including registration and export of the 14 required metrics.
 */

#ifndef AIRY_RT_PROMETHEUS_EXPORTER_H
#define AIRY_RT_PROMETHEUS_EXPORTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief C-L10: initialize the Prometheus exporter.
 *
 * Initializes the unified metrics collector and registers the 14 required
 * metrics.
 * @param service_name Service name (e.g. "monit_d")
 * @return 0 on success, non-zero on failure
 */
int prometheus_exporter_init(const char *service_name);

/** @brief C-L10: shut down the Prometheus exporter. */
void prometheus_exporter_shutdown(void);


/**
 * @brief C-L10: register the 14 required metrics.
 *
 * Registers the 10 core metrics defined in engineering-standards manual
 * 16.1 plus the 4 memory-observability metrics.
 * @return 0 on success, non-zero on failure
 */
int prometheus_exporter_register_required_metrics(void);


/**
 * @brief C-L10: handle a Prometheus scrape HTTP request.
 *
 * Detects whether this is a "GET /metrics" HTTP request; if so, responds
 * with Prometheus-format metrics.
 *
 * @param request Received request data
 * @param request_len Request data length
 * @param response Output parameter, HTTP response data (caller frees)
 * @param response_len Output parameter, response data length
 * @return 0 if handled (is a /metrics request), -1 for non-Prometheus
 *         requests (needs other handling)
 */
int prometheus_exporter_handle_http(const char *request, size_t request_len, char **response,
                                    size_t *response_len);


/** @brief Increment a counter metric. */
void prometheus_counter_inc(const char *name, double value);

/** @brief Set a gauge metric value. */
void prometheus_gauge_set(const char *name, double value);

/** @brief Record a histogram observation. */
void prometheus_histogram_observe(const char *name, double value);

/**
 * @brief Get the full Prometheus-format metrics export.
 * @return Prometheus-format string (caller frees), NULL on failure
 */
char *prometheus_exporter_get_metrics(void);


/**
 * @brief C-L10: get Prometheus scrape statistics.
 *
 * @param out_count  Output: total scrape count (may be NULL)
 * @param out_errors Output: scrape-error count (may be NULL)
 */
void prometheus_exporter_get_scrape_stats(uint64_t *out_count, uint64_t *out_errors);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_PROMETHEUS_EXPORTER_H */