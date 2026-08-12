// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_discovery_stats.c
 * @brief 跨进程服务发现统计域：事件回调注册、统计查询/导出、
 *        服务计数、策略字符串映射与统计摘要输出
 */

#include "service_discovery_internal.h"

AIRY_API airy_err_t sd_register_event_callback(service_discovery_t sd_handle,
                                               sd_event_callback_t callback, void *user_data)
{
    if (!sd_handle || !callback)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    if (sd->callback_count >= SD_MAX_CALLBACKS) {
        airy_mtx_unlock(&sd->mutex);
        return AIRY_ENOMEM;
    }

    sd->callbacks[sd->callback_count].callback = callback;
    sd->callbacks[sd->callback_count].user_data = user_data;
    sd->callback_count++;

    airy_mtx_unlock(&sd->mutex);

    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_get_stats(service_discovery_t sd_handle, sd_stats_t *stats)
{
    if (!sd_handle || !stats)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);
    sd->backend->refresh(sd, NULL);
    __builtin_memcpy(stats, &sd->stats, sizeof(sd_stats_t));
    stats->active_services = sd->service_count;
    stats->active_instances = 0;
    for (uint32_t i = 0; i < sd->service_count; i++) {
        stats->active_instances += sd->services[i].instance_count;
    }
    airy_mtx_unlock(&sd->mutex);

    return AIRY_SUCCESS;
}

AIRY_API uint32_t sd_service_count(service_discovery_t sd_handle)
{
    if (!sd_handle)
        return 0;
    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);
    sd->backend->refresh(sd, NULL);
    uint32_t count = sd->service_count;
    airy_mtx_unlock(&sd->mutex);

    return count;
}

AIRY_API const char *sd_lb_strategy_to_string(sd_lb_strategy_t strategy)
{
    static const char *strategy_strings[] = {"ROUND_ROBIN", "WEIGHTED", "LEAST_CONNECTION",
                                             "RANDOM", "LEAST_LOAD"};

    if (strategy < 0 || strategy > SD_LB_LEAST_LOAD)
        return "UNKNOWN";
    return strategy_strings[strategy];
}

AIRY_API void sd_dump_stats(service_discovery_t sd_handle)
{
    if (!sd_handle) {
        SD_LOG_WARN("STATS unavailable (NULL handle)");
        return;
    }

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    sd->backend->refresh(sd, NULL);

    sd_stats_t stats = sd->stats;
    stats.active_services = sd->service_count;
    stats.active_instances = 0;
    for (uint32_t i = 0; i < sd->service_count; i++) {
        stats.active_instances += sd->services[i].instance_count;
    }

    uint32_t healthy_instances = 0;
    for (uint32_t i = 0; i < sd->service_count; i++) {
        for (uint32_t j = 0; j < sd->services[i].instance_count; j++) {
            if (sd->services[i].instances[j].healthy)
                healthy_instances++;
        }
    }

    airy_mtx_unlock(&sd->mutex);

    SD_LOG_INFO("SD-STATS services=%u instances=%u (%u healthy) "
                "registrations=%llu deregistrations=%llu "
                "discoveries=%llu heartbeats=%llu "
                "expirations=%llu lb_selections=%llu "
                "running=%s",
                stats.active_services, stats.active_instances, healthy_instances,
                (unsigned long long)stats.registrations, (unsigned long long)stats.deregistrations,
                (unsigned long long)stats.discoveries, (unsigned long long)stats.heartbeats,
                (unsigned long long)stats.expirations, (unsigned long long)stats.lb_selections,
                sd->running ? "yes" : "no");
}
