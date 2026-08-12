// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_discovery_lb.c
 * @brief 跨进程服务发现负载均衡域：轮询/加权/最少连接/随机/最少负载
 *        实例选择策略实现
 */

#include "service_discovery_internal.h"

airy_err_t lb_round_robin(sd_internal_t *sd, const sd_service_entry_t *entry,
                          sd_instance_t *result)
{
    if (entry->instance_count == 0) {
        AIRY_ERROR(AIRY_ENOENT, "service_discovery: endpoint not found");
    }

    uint32_t start = sd->rr_counter % entry->instance_count;
    for (uint32_t i = 0; i < entry->instance_count; i++) {
        uint32_t idx = (start + i) % entry->instance_count;
        if (entry->instances[idx].healthy) {
            __builtin_memcpy(result, &entry->instances[idx], sizeof(sd_instance_t));
            sd->rr_counter = idx + 1;
            return AIRY_SUCCESS;
        }
    }
    return AIRY_ENOENT;
}

airy_err_t lb_weighted(const sd_service_entry_t *entry, sd_instance_t *result)
{
    uint32_t total_weight = 0;
    for (uint32_t i = 0; i < entry->instance_count; i++) {
        if (entry->instances[i].healthy) {
            total_weight += entry->instances[i].weight;
        }
    }
    if (total_weight == 0) {
        AIRY_ERROR(AIRY_ENOENT, "service_discovery: no endpoints registered");
    }

    uint32_t random_val = airy_random_uint32(0, total_weight - 1);
    uint32_t cumulative = 0;
    for (uint32_t i = 0; i < entry->instance_count; i++) {
        if (!entry->instances[i].healthy)
            continue;
        cumulative += entry->instances[i].weight;
        if (random_val < cumulative) {
            __builtin_memcpy(result, &entry->instances[i], sizeof(sd_instance_t));
            return AIRY_SUCCESS;
        }
    }

    for (uint32_t i = 0; i < entry->instance_count; i++) {
        if (entry->instances[i].healthy) {
            __builtin_memcpy(result, &entry->instances[i], sizeof(sd_instance_t));
            return AIRY_SUCCESS;
        }
    }
    AIRY_ERROR(AIRY_ENOENT, "service_discovery: service not registered");
}

airy_err_t lb_least_connection(const sd_service_entry_t *entry, sd_instance_t *result)
{
    int32_t best_idx = -1;
    uint32_t min_conn = UINT32_MAX;

    for (uint32_t i = 0; i < entry->instance_count; i++) {
        if (!entry->instances[i].healthy)
            continue;
        if (entry->instances[i].active_connections < min_conn) {
            min_conn = entry->instances[i].active_connections;
            best_idx = (int32_t)i;
        }
    }

    if (best_idx < 0) {
        AIRY_ERROR(AIRY_ENOENT, "service_discovery: health check failed");
    }
    __builtin_memcpy(result, &entry->instances[best_idx], sizeof(sd_instance_t));
    return AIRY_SUCCESS;
}

airy_err_t lb_random(const sd_service_entry_t *entry, sd_instance_t *result)
{
    uint32_t healthy_count = 0;
    for (uint32_t i = 0; i < entry->instance_count; i++) {
        if (entry->instances[i].healthy)
            healthy_count++;
    }
    if (healthy_count == 0)
        return AIRY_ENOENT;

    uint32_t idx = airy_random_uint32(0, healthy_count - 1);
    uint32_t count = 0;
    for (uint32_t i = 0; i < entry->instance_count; i++) {
        if (entry->instances[i].healthy) {
            if (count == idx) {
                __builtin_memcpy(result, &entry->instances[i], sizeof(sd_instance_t));
                return AIRY_SUCCESS;
            }
            count++;
        }
    }
    return AIRY_ENOENT;
}

airy_err_t lb_least_load(const sd_service_entry_t *entry, sd_instance_t *result)
{
    int32_t best_idx = -1;
    uint32_t min_load = UINT32_MAX;

    for (uint32_t i = 0; i < entry->instance_count; i++) {
        if (!entry->instances[i].healthy)
            continue;
        uint32_t load =
            entry->instances[i].max_connections > 0 ?
                entry->instances[i].active_connections * 100 / entry->instances[i].max_connections :
                0;
        if (load < min_load) {
            min_load = load;
            best_idx = (int32_t)i;
        }
    }

    if (best_idx < 0)
        return AIRY_ENOENT;
    __builtin_memcpy(result, &entry->instances[best_idx], sizeof(sd_instance_t));
    return AIRY_SUCCESS;
}
