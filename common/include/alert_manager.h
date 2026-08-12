/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file alert_manager.h
 * @brief Intelligent alert management system.
 *
 * Provides unified alert management supporting:
 * - Multi-level alerts (info/warning/critical/emergency)
 * - Alert rule engine (threshold/trend/composite conditions)
 * - Alert suppression and deduplication
 * - Multi-channel notification (log/callback/webhook)
 * - Alert escalation policy
 * - Integration with the circuit breaker and service discovery
 *
 * @see circuit_breaker.h  circuit breaker
 * @see service_discovery.h  service discovery
 */

#ifndef AIRY_RT_ALERT_MANAGER_H
#define AIRY_RT_ALERT_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#define AM_MAX_RULES 64
#define AM_MAX_NAME_LEN 64
#define AM_MAX_MESSAGE_LEN 512
#define AM_MAX_CHANNELS 8
#define AM_MAX_ACTIVE_ALERTS 256


typedef enum {
    AM_LEVEL_INFO = 0,
    AM_LEVEL_WARNING = 1,
    AM_LEVEL_CRITICAL = 2,
    AM_LEVEL_EMERGENCY = 3
} am_level_t;


typedef enum {
    AM_STATE_PENDING = 0,
    AM_STATE_FIRING = 1,
    AM_STATE_RESOLVED = 2,
    AM_STATE_SUPPRESSED = 3,
    AM_STATE_ACKNOWLEDGED = 4
} am_state_t;


typedef enum {
    AM_RULE_THRESHOLD = 0,
    AM_RULE_TREND = 1,
    AM_RULE_COMPOSITE = 2,
    AM_RULE_ANOMALY = 3
} am_rule_type_t;


typedef enum {
    AM_OP_GT = 0,
    AM_OP_GTE = 1,
    AM_OP_LT = 2,
    AM_OP_LTE = 3,
    AM_OP_EQ = 4,
    AM_OP_NEQ = 5
} am_comparison_t;


typedef enum {
    AM_CHANNEL_LOG = 0,
    AM_CHANNEL_CALLBACK = 1,
    AM_CHANNEL_WEBHOOK = 2,
    AM_CHANNEL_FILE = 3
} am_channel_type_t;


typedef struct {
    char name[AM_MAX_NAME_LEN];
    am_level_t level;
    am_state_t state;
    char message[AM_MAX_MESSAGE_LEN];
    char source[64];
    char labels[256];
    uint64_t fired_at;
    uint64_t resolved_at;
    uint64_t last_notified;
    uint32_t notification_count;
    uint32_t trigger_count;
    bool acknowledged;
} am_alert_t;


typedef struct {
    char name[AM_MAX_NAME_LEN];
    am_rule_type_t type;
    am_level_t level;
    char metric_name[128];
    am_comparison_t comparison;
    double threshold;
    uint32_t duration_seconds;
    uint32_t cooldown_seconds;
    char composite_expr[256];
    bool enabled;
    uint64_t last_triggered;
} am_rule_t;


typedef struct {
    am_channel_type_t type;
    char name[AM_MAX_NAME_LEN];
    char config[512];
    am_level_t min_level;
    bool enabled;
} am_channel_t;


typedef struct {
    uint32_t evaluation_interval_ms;
    uint32_t default_cooldown_ms;
    uint32_t max_notifications_per_alert;
    uint32_t escalation_timeout_ms;
    bool enable_deduplication;
    bool enable_suppression;
} am_config_t;


typedef void (*am_alert_callback_t)(const am_alert_t *alert, void *user_data);


/**
 * @brief Create the alert manager.
 * @param config Config params (NULL = defaults)
 * @return 0 on success, non-zero on failure
 */
int am_init(const am_config_t *config);

/** @brief Shut down the alert manager. */
void am_shutdown(void);


/**
 * @brief Add an alert rule.
 * @param rule Rule definition
 * @return 0 on success, non-zero on failure
 */
int am_add_rule(const am_rule_t *rule);

/**
 * @brief Remove an alert rule.
 * @param name Rule name
 * @return 0 on success, non-zero on failure
 */
int am_remove_rule(const char *name);

/**
 * @brief Enable/disable a rule.
 * @param name Rule name
 * @param enabled Whether enabled
 * @return 0 on success, non-zero on failure
 */
int am_set_rule_enabled(const char *name, bool enabled);


/**
 * @brief Fire an alert.
 * @param name Alert name
 * @param level Alert level
 * @param message Alert message
 * @param source Source
 * @param labels Labels
 * @return 0 on success, non-zero on failure
 */
int am_fire(const char *name, am_level_t level, const char *message, const char *source,
            const char *labels);

/**
 * @brief Resolve an alert.
 * @param name Alert name
 * @return 0 on success, non-zero on failure
 */
int am_resolve(const char *name);

/**
 * @brief Acknowledge an alert.
 * @param name Alert name
 * @return 0 on success, non-zero on failure
 */
int am_acknowledge(const char *name);


/**
 * @brief Record a metric value (for am_evaluate_all).
 * @param metric_name Metric name
 * @param value Current metric value
 * @return 0 on success, non-zero on failure
 */
int am_record_metric(const char *metric_name, double value);

/**
 * @brief Evaluate a metric value (checks whether rules trigger).
 * @param metric_name Metric name
 * @param value Metric value
 * @return Number of triggered alerts (0 = none)
 */
int am_evaluate(const char *metric_name, double value);

/**
 * @brief Evaluate all rules.
 * @return Number of triggered alerts
 */
int am_evaluate_all(void);


/**
 * @brief Register a notification channel.
 * @param channel Channel config
 * @return 0 on success, non-zero on failure
 */
int am_register_channel(const am_channel_t *channel);

/**
 * @brief Register an alert callback.
 * @param callback Callback function
 * @param user_data User data
 * @param min_level Minimum alert level
 * @return 0 on success, non-zero on failure
 */
int am_register_callback(am_alert_callback_t callback, void *user_data, am_level_t min_level);


/**
 * @brief Get the active alert list.
 * @param alerts [out] Alert array
 * @param max_count Array capacity
 * @param found_count [out] Actual count
 * @return 0 on success, non-zero on failure
 */
int am_get_active_alerts(am_alert_t *alerts, uint32_t max_count, uint32_t *found_count);

/**
 * @brief Get active alerts of the given level.
 * @param level Alert level
 * @param alerts [out] Alert array
 * @param max_count Array capacity
 * @param found_count [out] Actual count
 * @return 0 on success, non-zero on failure
 */
int am_get_alerts_by_level(am_level_t level, am_alert_t *alerts, uint32_t max_count,
                           uint32_t *found_count);

/**
 * @brief Get the number of active alerts.
 * @return Active alert count
 */
uint32_t am_active_alert_count(void);


/**
 * @brief Convert an alert level to a string.
 * @param level Level
 * @return Level name
 */
const char *am_level_to_string(am_level_t level);

/**
 * @brief Convert an alert state to a string.
 * @param state State
 * @return State name
 */
const char *am_state_to_string(am_state_t state);

/**
 * @brief Create a default config.
 * @return Default config
 */
am_config_t am_create_default_config(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_ALERT_MANAGER_H */
