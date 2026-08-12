// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_discovery.c
 * @brief 跨进程服务发现机制实现
 *
 * 基于共享内存的跨进程服务注册中心实现。
 *
 * @see service_discovery.h
 */

#include "service_discovery.h"

#include "daemon_errors.h"
#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "safe_string_utils.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include "error.h"

#if AIRY_PLATFORM_POSIX
#include <dirent.h>
#endif

#include "service_discovery_internal.h"

int32_t find_service_index(sd_internal_t *sd, const char *name)
{
    for (uint32_t i = 0; i < sd->service_count; i++) {
        if (strcmp(sd->services[i].name, name) == 0)
            return (int32_t)i;
    }
    /* "not found" is normal control flow (the caller judges via the return
     * value), not an error. Previously calling AIRY_ERROR_HANDLE allocated an
     * error context on every lookup miss, causing memory leaks (especially
     * under concurrent registration). */
    return AIRY_ERR_NOT_FOUND;
}

int32_t find_instance_index(sd_service_entry_t *entry, const char *instance_id)
{
    for (uint32_t i = 0; i < entry->instance_count; i++) {
        if (strcmp(entry->instances[i].instance_id, instance_id) == 0)
            return (int32_t)i;
    }

    return AIRY_ERR_NOT_FOUND;
}

void notify_event(sd_internal_t *sd, sd_event_type_t event, const char *service_name,
                  const sd_instance_t *instance)
{
    for (uint32_t i = 0; i < sd->callback_count; i++) {
        if (sd->callbacks[i].callback) {
            sd->callbacks[i].callback(event, service_name, instance, sd->callbacks[i].user_data);
        }
    }
}

bool is_instance_expired(const sd_instance_t *inst, uint32_t expire_ms)
{
    if (expire_ms == 0)
        return false;
    uint64_t now = airy_time_ms();
    return (now - inst->last_heartbeat) > expire_ms;
}

void expire_stale_instances(sd_internal_t *sd)
{
    if (!sd->config.enable_auto_expire)
        return;

    uint64_t now = airy_time_ms();
    bool changed = false;
    for (uint32_t i = 0; i < sd->service_count; i++) {
        sd_service_entry_t *entry = &sd->services[i];
        for (uint32_t j = 0; j < entry->instance_count;) {
            if (is_instance_expired(&entry->instances[j], sd->config.expire_timeout_ms)) {
                sd_instance_t expired = entry->instances[j];
                SD_LOG_WARN("EXPIRED instance='%s' service='%s' "
                            "last_heartbeat=%llums ago "
                            "(active_svcs=%u active_insts=%u)",
                            expired.instance_id, entry->name,
                            (unsigned long long)(now - expired.last_heartbeat), sd->service_count,
                            entry->instance_count);

                if (j < entry->instance_count - 1) {
                    entry->instances[j] = entry->instances[entry->instance_count - 1];
                }
                __builtin_memset(&entry->instances[entry->instance_count - 1], 0,
                                 sizeof(sd_instance_t));
                entry->instance_count--;
                sd->stats.expirations++;
                changed = true;

                notify_event(sd, SD_EVENT_EXPIRED, entry->name, &expired);
            } else {
                j++;
            }
        }

        if (entry->instance_count == 0 && sd->config.enable_auto_expire) {
            if (now - entry->last_updated > sd->config.expire_timeout_ms * 2) {
                if (i < sd->service_count - 1) {
                    sd->services[i] = sd->services[sd->service_count - 1];
                }
                __builtin_memset(&sd->services[sd->service_count - 1], 0,
                                 sizeof(sd_service_entry_t));
                sd->service_count--;
                changed = true;
                i--;
            }
        }
    }

    if (changed && sd->backend)
        sd->backend->commit(sd, NULL);
}

airy_err_t sd_registry_add_instance(sd_internal_t *sd, const char *name, const char *type,
                                    const sd_instance_t *inst, const char *tags,
                                    const char *deps)
{
    int32_t svc_idx = find_service_index(sd, name);
    sd_service_entry_t *entry = NULL;

    if (svc_idx >= 0) {
        entry = &sd->services[svc_idx];
    } else {
        if (sd->service_count >= SD_MAX_SERVICES)
            return AIRY_ENOMEM;
        entry = &sd->services[sd->service_count];
        __builtin_memset(entry, 0, sizeof(sd_service_entry_t));
        safe_strcpy(entry->name, name, SD_MAX_NAME_LEN);
        safe_strcpy(entry->service_type, type, SD_MAX_TYPE_LEN);
        if (tags)
            safe_strcpy(entry->tags, tags, SD_MAX_TAGS_LEN);
        if (deps)
            safe_strcpy(entry->dependencies, deps, SD_MAX_DEPS_LEN);
        entry->active = true;
        entry->last_updated = airy_time_ms();
        sd->service_count++;
        sd->stats.registrations++;
    }

    int32_t inst_idx = find_instance_index(entry, inst->instance_id);
    if (inst_idx >= 0) {
        __builtin_memcpy(&entry->instances[inst_idx], inst, sizeof(sd_instance_t));
        entry->instances[inst_idx].last_heartbeat = airy_time_ms();
        entry->instances[inst_idx].register_time = entry->instances[inst_idx].register_time > 0 ?
                                                       entry->instances[inst_idx].register_time :
                                                       airy_time_ms();
    } else {
        if (entry->instance_count >= SD_MAX_INSTANCES)
            return AIRY_ENOMEM;
        __builtin_memcpy(&entry->instances[entry->instance_count], inst, sizeof(sd_instance_t));
        entry->instances[entry->instance_count].last_heartbeat = airy_time_ms();
        entry->instances[entry->instance_count].register_time = airy_time_ms();
        entry->instances[entry->instance_count].pid =
#ifdef _WIN32
            (uint32_t)GetCurrentProcessId();
#else
            (uint32_t)getpid();
#endif
        entry->instance_count++;
    }

    entry->last_updated = airy_time_ms();
    sd->stats.active_services = sd->service_count;
    sd->stats.active_instances = 0;
    for (uint32_t i = 0; i < sd->service_count; i++)
        sd->stats.active_instances += sd->services[i].instance_count;
    return AIRY_SUCCESS;
}

airy_err_t sd_registry_remove_instance(sd_internal_t *sd, const char *name,
                                       const char *instance_id, sd_instance_t *out_removed)
{
    int32_t svc_idx = find_service_index(sd, name);
    if (svc_idx < 0)
        return AIRY_ENOENT;
    sd_service_entry_t *entry = &sd->services[svc_idx];
    int32_t inst_idx = find_instance_index(entry, instance_id);
    if (inst_idx < 0)
        return AIRY_ENOENT;

    sd_instance_t removed = entry->instances[inst_idx];
    if ((uint32_t)inst_idx < entry->instance_count - 1)
        entry->instances[inst_idx] = entry->instances[entry->instance_count - 1];
    __builtin_memset(&entry->instances[entry->instance_count - 1], 0, sizeof(sd_instance_t));
    entry->instance_count--;
    entry->last_updated = airy_time_ms();

    sd->stats.deregistrations++;
    sd->stats.active_instances = 0;
    for (uint32_t i = 0; i < sd->service_count; i++)
        sd->stats.active_instances += sd->services[i].instance_count;

    if (out_removed)
        *out_removed = removed;
    return AIRY_SUCCESS;
}

const sd_backend_t *sd_backend_select(void)
{
#ifdef AIRY_HAS_CJSON
    const char *env = getenv("AIRY_SD_BACKEND");
    if (env && (strcmp(env, "file") == 0 || strcmp(env, "filesystem") == 0))
        return &sd_backend_file;
#endif
    return &sd_backend_shm;
}

AIRY_API sd_config_t sd_create_default_config(void)
{
    sd_config_t config;
    __builtin_memset(&config, 0, sizeof(sd_config_t));
    config.heartbeat_interval_ms = SD_DEFAULT_HEARTBEAT_MS;
    config.expire_timeout_ms = SD_DEFAULT_EXPIRE_MS;
    config.default_lb_strategy = SD_LB_ROUND_ROBIN;
    config.enable_auto_expire = true;
    config.enable_health_propagation = true;
    safe_strcpy(config.shm_name, SD_SHM_NAME, sizeof(config.shm_name));
    config.shm_size = SD_SHM_DEFAULT_SIZE;
    return config;
}

AIRY_API service_discovery_t sd_create(const sd_config_t *config)
{
    sd_internal_t *sd = (sd_internal_t *)AIRY_CALLOC(1, sizeof(sd_internal_t));
    if (!sd) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    if (config) {
        __builtin_memcpy(&sd->config, config, sizeof(sd_config_t));
    } else {
        sd->config = sd_create_default_config();
    }

    airy_err_t err = airy_mtx_init(&sd->mutex);
    if (err != AIRY_SUCCESS) {
        AIRY_FREE(sd);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    sd->running = false;
    sd->rr_counter = 0;
    sd->shm_handle = NULL;
    sd->shm_ptr = NULL;
    sd->is_shm_owner = false;

    sd->backend = sd_backend_select();
    if (sd->backend->init(sd) != AIRY_SUCCESS) {
        airy_mtx_destroy(&sd->mutex);
        AIRY_FREE(sd);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "backend init failed");
    }

    SD_LOG_INFO("CREATE (heartbeat=%ums expire=%ums lb=%s backend=%s)",
                sd->config.heartbeat_interval_ms, sd->config.expire_timeout_ms,
                sd_lb_strategy_to_string(sd->config.default_lb_strategy), sd->backend->name);
    return (service_discovery_t)sd;
}

AIRY_API void sd_destroy(service_discovery_t sd_handle)
{
    if (!sd_handle)
        return;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    if (sd->running) {
        sd_stop(sd_handle);
    }

    if (sd->shm_ptr) {
        sd->shm_ptr = NULL;
    }

    if (sd->backend)
        sd->backend->deinit(sd);

    airy_mtx_destroy(&sd->mutex);
    AIRY_FREE(sd);

    SD_LOG_INFO("DESTROY");
}

AIRY_API airy_err_t sd_start(service_discovery_t sd_handle)
{
    if (!sd_handle)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);
    if (sd->running) {
        airy_mtx_unlock(&sd->mutex);
        return AIRY_SUCCESS;
    }

    sd->running = true;
    airy_mtx_unlock(&sd->mutex);

    SD_LOG_INFO("START (heartbeat=%ums expire=%ums)", sd->config.heartbeat_interval_ms,
                sd->config.expire_timeout_ms);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_stop(service_discovery_t sd_handle)
{
    if (!sd_handle)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);
    sd->running = false;
    airy_mtx_unlock(&sd->mutex);

    SD_LOG_INFO("STOP");
    return AIRY_SUCCESS;
}

AIRY_API bool sd_is_running(service_discovery_t sd_handle)
{
    if (!sd_handle)
        return false;
    sd_internal_t *sd = (sd_internal_t *)sd_handle;
    return sd->running;
}
