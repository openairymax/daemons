// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_monitor.c
 * @brief Service monitoring and degradation handling (monitor + degradation domain).
 *
 * Implements the service-monitoring interface defined in svc_common.h
 * (airy_svc_monitor_start/stop) and degradation-handler registration
 * (airy_svc_set_degrad_hdlr). A monitoring thread periodically runs health
 * checks, triggering the degradation callback when consecutive failures
 * exceed the threshold, with configurable auto-restart (with exponential
 * backoff).
 *
 * monitor and degradation share the same monitoring table (g_monitor):
 * - airy_svc_monitor_start creates the monitor thread and initializes the
 *   degradation fields
 * - airy_svc_set_degrad_hdlr registers degradation fields directly for
 *   unmonitored services
 * They depend on each other, so both live in this file.
 *
 * Cross-module interface: monitor_shutdown() (non-static) is called by
 * airy_svc_common_cleanup() in svc_common.c on the exit path; declared in
 * svc_common_internal.h; g_monitor state stays private to this file.
 *
 * @see agentrt/daemons/common/include/svc_common.h
 * @see agentrt/daemons/common/src/svc_common.c
 * @see agentrt/daemons/common/src/svc_common_internal.h
 */

#include "svc_common.h"

#include "atomic_compat.h"
#include "daemon_errors.h"
#include "daemon_platform_ext.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

#define MAX_MONITORED_SERVICES 32
typedef struct {
    airy_svc_t service;
    airy_monitor_config_t config;
    airy_degradation_handler_t degradation_handler;
    void *degradation_user_data;
    bool active;
    uint32_t consecutive_failures;
    uint32_t restart_attempts;
    uint64_t last_check_time;
    uint64_t next_restart_time;
    bool degraded;
    airy_thread_t monitor_thread;
    atomic_int stop_requested;
} monitored_service_t;

static struct {
    monitored_service_t services[MAX_MONITORED_SERVICES];
    uint32_t count;
    bool initialized;
    airy_mtx_t mutex;
} g_monitor;

/* Not static: called by airy_svc_common_cleanup() in svc_common.c (exit
 * path); declared in svc_common_internal.h. */
void monitor_shutdown(void)
{
    if (g_monitor.initialized) {
        airy_mtx_destroy(&g_monitor.mutex);
        g_monitor.initialized = false;
        SVC_LOG_INFO("Monitor: mutex destroyed");
    }
}

static airy_err_t monitor_init(void)
{
    if (g_monitor.initialized) {
        return AIRY_SUCCESS;
    }

    airy_err_t err = airy_mtx_init(&g_monitor.mutex);
    if (err != AIRY_SUCCESS) {
        return err;
    }

    __builtin_memset(g_monitor.services, 0, sizeof(g_monitor.services));
    g_monitor.count = 0;
    g_monitor.initialized = true;

    return AIRY_SUCCESS;
}

static void *monitor_thread_func(void *arg)
{
    monitored_service_t *mon = (monitored_service_t *)arg;
    if (!mon || !mon->service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    const char *svc_name = airy_svc_get_name(mon->service);
    uint32_t interval_ms = mon->config.healthcheck_interval_ms;
    if (interval_ms == 0)
        interval_ms = 30000;

    LOG_INFO("Monitor thread started for service '%s' (interval=%ums)", svc_name, interval_ms);

    while (!mon->stop_requested && mon->active) {
        airy_sleep_ms(interval_ms);

        if (mon->stop_requested || !mon->active)
            break;

        airy_err_t err = airy_svc_healthcheck(mon->service);
        mon->last_check_time = airy_time_ms();

        if (err != AIRY_SUCCESS) {
            mon->consecutive_failures++;
            LOG_WARN("Service '%s' health check failed (consecutive: %u)", svc_name,
                     mon->consecutive_failures);

            if (mon->config.enable_degradation &&
                mon->consecutive_failures >= mon->config.degradation_threshold && !mon->degraded &&
                mon->degradation_handler) {
                mon->degraded = true;
                char reason[128];
                snprintf(reason, sizeof(reason), "consecutive_failures=%u >= threshold=%u",
                         mon->consecutive_failures, mon->config.degradation_threshold);
                mon->degradation_handler(mon->service, reason, mon->degradation_user_data);
                LOG_WARN("Service '%s' degraded: %s", svc_name, reason);
            }

            if (mon->config.auto_restart &&
                mon->restart_attempts < mon->config.max_restart_attempts) {
                uint64_t now = airy_time_ms();
                if (now >= mon->next_restart_time) {
                    mon->restart_attempts++;
                    LOG_INFO("Auto-restarting service '%s' (attempt %u/%u)", svc_name,
                             mon->restart_attempts, mon->config.max_restart_attempts);
                    airy_svc_stop(mon->service, true);
                    airy_err_t start_err = airy_svc_start(mon->service);
                    if (start_err == AIRY_SUCCESS) {
                        mon->consecutive_failures = 0;
                        mon->degraded = false;
                        LOG_INFO("Service '%s' restarted successfully", svc_name);
                    } else {
                        uint32_t backoff = mon->config.restart_backoff_base_ms *
                                           (1 << (mon->restart_attempts - 1));
                        if (backoff > mon->config.restart_backoff_max_ms)
                            backoff = mon->config.restart_backoff_max_ms;
                        mon->next_restart_time = now + backoff;
                        LOG_ERROR("Service '%s' restart failed, next retry in %ums", svc_name,
                                  backoff);
                    }
                }
            }
        } else {
            if (mon->consecutive_failures > 0)
                LOG_INFO("Service '%s' recovered after %u failures", svc_name,
                         mon->consecutive_failures);
            mon->consecutive_failures = 0;
            mon->restart_attempts = 0;
            mon->degraded = false;
        }
    }

    LOG_INFO("Monitor thread stopped for service '%s'", svc_name);
    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

airy_err_t airy_svc_monitor_start(airy_svc_t service, const airy_monitor_config_t *config)
{
    if (!service || !config) {
        return AIRY_EINVAL;
    }

    monitor_init();

    airy_mtx_lock(&g_monitor.mutex);

    for (uint32_t i = 0; i < g_monitor.count; i++) {
        if (g_monitor.services[i].service == service) {
            g_monitor.services[i].stop_requested = 1;
            if (g_monitor.services[i].monitor_thread) {
                airy_mtx_unlock(&g_monitor.mutex);
                airy_thread_join(g_monitor.services[i].monitor_thread, NULL);
                airy_mtx_lock(&g_monitor.mutex);
            }
            __builtin_memcpy(&g_monitor.services[i].config, config, sizeof(airy_monitor_config_t));
            g_monitor.services[i].active = true;
            g_monitor.services[i].consecutive_failures = 0;
            g_monitor.services[i].restart_attempts = 0;
            g_monitor.services[i].degraded = false;
            g_monitor.services[i].stop_requested = 0;
            g_monitor.services[i].last_check_time = airy_time_ms();
            g_monitor.services[i].next_restart_time = 0;

            int thread_err = airy_thread_create(&g_monitor.services[i].monitor_thread,
                                                monitor_thread_func, &g_monitor.services[i]);
            if (thread_err != 0) {
                g_monitor.services[i].active = false;
                airy_mtx_unlock(&g_monitor.mutex);
                LOG_ERROR("Failed to create monitor thread for service '%s'",
                          airy_svc_get_name(service));
                return DAEMON_EINIT;
            }

            airy_mtx_unlock(&g_monitor.mutex);
            LOG_INFO("Service monitoring updated for '%s'", airy_svc_get_name(service));
            return AIRY_SUCCESS;
        }
    }

    if (g_monitor.count >= MAX_MONITORED_SERVICES) {
        airy_mtx_unlock(&g_monitor.mutex);
        return AIRY_ENOMEM;
    }

    monitored_service_t *mon = &g_monitor.services[g_monitor.count];
    mon->service = service;
    __builtin_memcpy(&mon->config, config, sizeof(airy_monitor_config_t));
    mon->degradation_handler = NULL;
    mon->degradation_user_data = NULL;
    mon->active = true;
    mon->consecutive_failures = 0;
    mon->restart_attempts = 0;
    mon->last_check_time = airy_time_ms();
    mon->next_restart_time = 0;
    mon->degraded = false;
    mon->stop_requested = 0;
    mon->monitor_thread = (airy_thread_t)0;

    int thread_err = airy_thread_create(&mon->monitor_thread, monitor_thread_func, mon);
    if (thread_err != 0) {
        mon->active = false;
        airy_mtx_unlock(&g_monitor.mutex);
        LOG_ERROR("Failed to create monitor thread for service '%s'", airy_svc_get_name(service));
        return DAEMON_EINIT;
    }

    g_monitor.count++;

    airy_mtx_unlock(&g_monitor.mutex);

    LOG_INFO("Service monitoring started for '%s' (interval=%ums, auto_restart=%s)",
             airy_svc_get_name(service), config->healthcheck_interval_ms,
             config->auto_restart ? "true" : "false");
    return AIRY_SUCCESS;
}

airy_err_t airy_svc_monitor_stop(airy_svc_t service)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    if (!g_monitor.initialized) {
        return AIRY_ENOTINIT;
    }

    airy_mtx_lock(&g_monitor.mutex);

    for (uint32_t i = 0; i < g_monitor.count; i++) {
        if (g_monitor.services[i].service == service) {
            g_monitor.services[i].stop_requested = 1;
            g_monitor.services[i].active = false;

            airy_thread_t thread = g_monitor.services[i].monitor_thread;

            airy_mtx_unlock(&g_monitor.mutex);

            if (thread) {
                airy_thread_join(thread, NULL);
            }

            airy_mtx_lock(&g_monitor.mutex);

            for (uint32_t j = 0; j < g_monitor.count; j++) {
                if (g_monitor.services[j].service == service) {
                    LOG_INFO("Service monitoring stopped for '%s'", airy_svc_get_name(service));
                    if (j < g_monitor.count - 1) {
                        g_monitor.services[j] = g_monitor.services[g_monitor.count - 1];
                    }
                    __builtin_memset(&g_monitor.services[g_monitor.count - 1], 0,
                                     sizeof(monitored_service_t));
                    g_monitor.count--;
                    break;
                }
            }

            airy_mtx_unlock(&g_monitor.mutex);
            return AIRY_SUCCESS;
        }
    }

    airy_mtx_unlock(&g_monitor.mutex);
    return AIRY_ENOENT;
}

airy_err_t airy_svc_set_degrad_hdlr(airy_svc_t service, airy_degradation_handler_t handler,
                                    void *user_data)
{
    if (!service || !handler) {
        return AIRY_EINVAL;
    }

    monitor_init();

    airy_mtx_lock(&g_monitor.mutex);

    for (uint32_t i = 0; i < g_monitor.count; i++) {
        if (g_monitor.services[i].service == service) {
            g_monitor.services[i].degradation_handler = handler;
            g_monitor.services[i].degradation_user_data = user_data;
            airy_mtx_unlock(&g_monitor.mutex);
            LOG_INFO("Degradation handler set for service '%s'", airy_svc_get_name(service));
            return AIRY_SUCCESS;
        }
    }

    if (g_monitor.count < MAX_MONITORED_SERVICES) {
        monitored_service_t *mon = &g_monitor.services[g_monitor.count];
        mon->service = service;
        mon->degradation_handler = handler;
        mon->degradation_user_data = user_data;
        mon->active = false;
        mon->consecutive_failures = 0;
        mon->restart_attempts = 0;
        mon->degraded = false;
        g_monitor.count++;

        airy_mtx_unlock(&g_monitor.mutex);
        LOG_INFO("Degradation handler set for unmonitored service '%s'",
                 airy_svc_get_name(service));
        return AIRY_SUCCESS;
    }

    airy_mtx_unlock(&g_monitor.mutex);
    return AIRY_ENOMEM;
}
