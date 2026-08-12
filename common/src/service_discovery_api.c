// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_discovery_api.c
 * @brief 跨进程服务发现操作域：服务注册/注销、发现、负载均衡选择、
 *        心跳、健康状态/连接数更新、依赖查询与检查等对外操作 API
 */

#include "service_discovery_internal.h"

AIRY_API airy_err_t sd_register(service_discovery_t sd_handle, const char *service_name,
                                const char *service_type, const sd_instance_t *instance,
                                const char *tags, const char *dependencies)
{
    if (!sd_handle || !service_name || !service_type || !instance)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);
    airy_err_t err =
        sd->backend->register_service(sd, service_name, service_type, instance, tags, dependencies);
    airy_mtx_unlock(&sd->mutex);

    if (err != AIRY_SUCCESS) {
        if (err == AIRY_ENOMEM)
            SD_LOG_ERROR("REGISTER failed (registry/instance full) service='%s'", service_name);
        return err;
    }

    notify_event(sd, SD_EVENT_REGISTERED, service_name, instance);

    SD_LOG_INFO("REGISTER service='%s' instance='%s' type='%s' "
                "endpoint='%s' (total_svcs=%u total_insts=%u)",
                service_name, instance->instance_id, service_type, instance->endpoint,
                sd->service_count, sd->stats.active_instances);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_deregister(service_discovery_t sd_handle, const char *service_name,
                                  const char *instance_id)
{
    if (!sd_handle || !service_name || !instance_id)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    sd_instance_t removed;
    __builtin_memset(&removed, 0, sizeof(removed));
    int32_t svc_idx = find_service_index(sd, service_name);
    if (svc_idx >= 0) {
        int32_t inst_idx = find_instance_index(&sd->services[svc_idx], instance_id);
        if (inst_idx >= 0)
            removed = sd->services[svc_idx].instances[inst_idx];
    }
    airy_err_t err = sd->backend->deregister_service(sd, service_name, instance_id);
    airy_mtx_unlock(&sd->mutex);

    if (err != AIRY_SUCCESS)
        return err;

    notify_event(sd, SD_EVENT_DEREGISTERED, service_name, &removed);

    SD_LOG_INFO("DEREGISTER service='%s' instance='%s' "
                "(total_svcs=%u total_insts=%u)",
                service_name, instance_id, sd->service_count, sd->stats.active_instances);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_deregister_all(service_discovery_t sd_handle, const char *service_name)
{
    if (!sd_handle || !service_name)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);
    airy_err_t err = sd->backend->deregister_all(sd, service_name);
    airy_mtx_unlock(&sd->mutex);

    if (err != AIRY_SUCCESS)
        return err;

    SD_LOG_INFO("DEREGISTER-ALL service='%s'", service_name);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_discover(service_discovery_t sd_handle, const char *service_name,
                                sd_instance_t *instances, uint32_t max_count, uint32_t *found_count)
{
    if (!sd_handle || !service_name || !instances || !found_count)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    sd->backend->refresh(sd, service_name);
    expire_stale_instances(sd);

    int32_t svc_idx = find_service_index(sd, service_name);
    if (svc_idx < 0) {
        *found_count = 0;
        airy_mtx_unlock(&sd->mutex);
        return AIRY_ENOENT;
    }

    sd_service_entry_t *entry = &sd->services[svc_idx];
    uint32_t count = 0;
    for (uint32_t i = 0; i < entry->instance_count && count < max_count; i++) {
        if (entry->instances[i].healthy) {
            __builtin_memcpy(&instances[count], &entry->instances[i], sizeof(sd_instance_t));
            count++;
        }
    }

    *found_count = count;
    sd->stats.discoveries++;

    airy_mtx_unlock(&sd->mutex);

    SD_LOG_DEBUG("DISCOVER service='%s' found=%u healthy", service_name, count);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_discover_by_type(service_discovery_t sd_handle, const char *service_type,
                                        sd_service_entry_t *entries, uint32_t max_count,
                                        uint32_t *found_count)
{
    if (!sd_handle || !service_type || !entries || !found_count)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    sd->backend->refresh(sd, NULL);
    expire_stale_instances(sd);

    uint32_t count = 0;
    for (uint32_t i = 0; i < sd->service_count && count < max_count; i++) {
        if (strcmp(sd->services[i].service_type, service_type) == 0) {
            __builtin_memcpy(&entries[count], &sd->services[i], sizeof(sd_service_entry_t));
            count++;
        }
    }

    *found_count = count;
    sd->stats.discoveries++;

    airy_mtx_unlock(&sd->mutex);

    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_discover_by_tags(service_discovery_t sd_handle, const char *tags,
                                        sd_service_entry_t *entries, uint32_t max_count,
                                        uint32_t *found_count)
{
    if (!sd_handle || !tags || !entries || !found_count)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    sd->backend->refresh(sd, NULL);
    expire_stale_instances(sd);

    char filter_copy[SD_MAX_TAGS_LEN];
    safe_strcpy(filter_copy, tags, sizeof(filter_copy));

    uint32_t count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(filter_copy, ",", &saveptr);

    while (token && count < max_count) {
        while (*token == ' ')
            token++;

        for (uint32_t i = 0; i < sd->service_count && count < max_count; i++) {
            if (strstr(sd->services[i].tags, token)) {
                bool already_added = false;
                for (uint32_t j = 0; j < count; j++) {
                    if (strcmp(entries[j].name, sd->services[i].name) == 0) {
                        already_added = true;
                        break;
                    }
                }
                if (!already_added) {
                    __builtin_memcpy(&entries[count], &sd->services[i], sizeof(sd_service_entry_t));
                    count++;
                }
            }
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    *found_count = count;
    sd->stats.discoveries++;

    airy_mtx_unlock(&sd->mutex);

    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_select_instance(service_discovery_t sd_handle, const char *service_name,
                                       sd_lb_strategy_t strategy, sd_instance_t *instance)
{
    if (!sd_handle || !service_name || !instance)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    sd->backend->refresh(sd, service_name);
    expire_stale_instances(sd);

    int32_t svc_idx = find_service_index(sd, service_name);
    if (svc_idx < 0) {
        airy_mtx_unlock(&sd->mutex);
        return AIRY_ENOENT;
    }

    sd_service_entry_t *entry = &sd->services[svc_idx];
    airy_err_t err;

    switch (strategy) {
    case SD_LB_ROUND_ROBIN:
        err = lb_round_robin(sd, entry, instance);
        break;
    case SD_LB_WEIGHTED:
        err = lb_weighted(entry, instance);
        break;
    case SD_LB_LEAST_CONNECTION:
        err = lb_least_connection(entry, instance);
        break;
    case SD_LB_RANDOM:
        err = lb_random(entry, instance);
        break;
    case SD_LB_LEAST_LOAD:
        err = lb_least_load(entry, instance);
        break;
    default:
        err = lb_round_robin(sd, entry, instance);
        break;
    }

    if (err == AIRY_SUCCESS) {
        sd->stats.lb_selections++;
    }

    airy_mtx_unlock(&sd->mutex);

    return err;
}

AIRY_API airy_err_t sd_heartbeat(service_discovery_t sd_handle, const char *service_name,
                                 const char *instance_id)
{
    if (!sd_handle || !service_name || !instance_id)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    sd->backend->refresh(sd, service_name);

    int32_t svc_idx = find_service_index(sd, service_name);
    if (svc_idx < 0) {
        airy_mtx_unlock(&sd->mutex);
        return AIRY_ENOENT;
    }

    sd_service_entry_t *entry = &sd->services[svc_idx];
    int32_t inst_idx = find_instance_index(entry, instance_id);
    if (inst_idx < 0) {
        airy_mtx_unlock(&sd->mutex);
        return AIRY_ENOENT;
    }

    entry->instances[inst_idx].last_heartbeat = airy_time_ms();
    sd->stats.heartbeats++;

    sd->backend->commit(sd, service_name);
    airy_mtx_unlock(&sd->mutex);

    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_update_health(service_discovery_t sd_handle, const char *service_name,
                                     const char *instance_id, bool healthy)
{
    if (!sd_handle || !service_name || !instance_id)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    sd->backend->refresh(sd, service_name);

    int32_t svc_idx = find_service_index(sd, service_name);
    if (svc_idx < 0) {
        airy_mtx_unlock(&sd->mutex);
        return AIRY_ENOENT;
    }

    sd_service_entry_t *entry = &sd->services[svc_idx];
    int32_t inst_idx = find_instance_index(entry, instance_id);
    if (inst_idx < 0) {
        airy_mtx_unlock(&sd->mutex);
        return AIRY_ENOENT;
    }

    bool was_healthy = entry->instances[inst_idx].healthy;
    entry->instances[inst_idx].healthy = healthy;
    entry->instances[inst_idx].last_heartbeat = airy_time_ms();
    entry->last_updated = airy_time_ms();

    sd->backend->commit(sd, service_name);
    airy_mtx_unlock(&sd->mutex);

    if (was_healthy != healthy) {
        sd_event_type_t event = healthy ? SD_EVENT_INSTANCE_UP : SD_EVENT_INSTANCE_DOWN;
        notify_event(sd, event, service_name, &entry->instances[inst_idx]);

        if (!healthy) {
            SD_LOG_WARN("UNHEALTHY instance='%s' service='%s'", instance_id, service_name);
        } else {
            SD_LOG_INFO("RECOVERED instance='%s' service='%s'", instance_id, service_name);
        }
    }

    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_update_connections(service_discovery_t sd_handle, const char *service_name,
                                          const char *instance_id, uint32_t active_connections)
{
    if (!sd_handle || !service_name || !instance_id)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    sd->backend->refresh(sd, service_name);

    int32_t svc_idx = find_service_index(sd, service_name);
    if (svc_idx < 0) {
        airy_mtx_unlock(&sd->mutex);
        return AIRY_ENOENT;
    }

    sd_service_entry_t *entry = &sd->services[svc_idx];
    int32_t inst_idx = find_instance_index(entry, instance_id);
    if (inst_idx < 0) {
        airy_mtx_unlock(&sd->mutex);
        return AIRY_ENOENT;
    }

    entry->instances[inst_idx].active_connections = active_connections;

    sd->backend->commit(sd, service_name);
    airy_mtx_unlock(&sd->mutex);

    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_get_dependencies(service_discovery_t sd_handle, const char *service_name,
                                        char *dependencies, size_t max_len)
{
    if (!sd_handle || !service_name || !dependencies)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    sd->backend->refresh(sd, service_name);

    int32_t svc_idx = find_service_index(sd, service_name);
    if (svc_idx < 0) {
        airy_mtx_unlock(&sd->mutex);
        return AIRY_ENOENT;
    }

    safe_strcpy(dependencies, sd->services[svc_idx].dependencies, (uint32_t)max_len);

    airy_mtx_unlock(&sd->mutex);

    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_check_dependencies(service_discovery_t sd_handle, const char *service_name,
                                          char *missing_deps, size_t max_len)
{
    if (!sd_handle || !service_name)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    sd->backend->refresh(sd, NULL);

    int32_t svc_idx = find_service_index(sd, service_name);
    if (svc_idx < 0) {
        airy_mtx_unlock(&sd->mutex);
        return AIRY_ENOENT;
    }

    char deps_copy[SD_MAX_DEPS_LEN];
    safe_strcpy(deps_copy, sd->services[svc_idx].dependencies, sizeof(deps_copy));

    char missing[SD_MAX_DEPS_LEN] = {0};
    size_t missing_len = 0;

    char *saveptr = NULL;
    char *token = strtok_r(deps_copy, ",", &saveptr);
    while (token) {
        while (*token == ' ')
            token++;

        int32_t dep_idx = find_service_index(sd, token);
        bool dep_available = false;
        if (dep_idx >= 0) {
            for (uint32_t i = 0; i < sd->services[dep_idx].instance_count; i++) {
                if (sd->services[dep_idx].instances[i].healthy) {
                    dep_available = true;
                    break;
                }
            }
        }

        if (!dep_available) {
            size_t token_len = strlen(token);
            if (missing_len + token_len + 2 < sizeof(missing)) {
                if (missing_len > 0) {
                    safe_strcat(missing, ",", sizeof(missing));
                    missing_len++;
                }
                safe_strcat(missing, token, sizeof(missing));
                missing_len += token_len;
            }
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    airy_mtx_unlock(&sd->mutex);

    if (missing_deps && max_len > 0) {
        safe_strcpy(missing_deps, missing, (uint32_t)max_len);
    }

    return missing_len > 0 ? DAEMON_EDEPEND : AIRY_SUCCESS;
}
