/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service_discovery_internal.h
 * @brief 跨进程服务发现内部跨文件共享声明（service_discovery.c 拆分后各域共用）
 */

#ifndef AIRY_RT_DAEMON_COMMON_SERVICE_DISCOVERY_INTERNAL_H
#define AIRY_RT_DAEMON_COMMON_SERVICE_DISCOVERY_INTERNAL_H

#include "service_discovery.h"

#include "daemon_errors.h"
#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "safe_string_utils.h"
#include "svc_logger.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_LOG_INFO(fmt, ...) LOG_INFO("C-L08: " fmt, ##__VA_ARGS__)
#define SD_LOG_WARN(fmt, ...) LOG_WARN("C-L08: " fmt, ##__VA_ARGS__)
#define SD_LOG_ERROR(fmt, ...) LOG_ERROR("C-L08: " fmt, ##__VA_ARGS__)
#define SD_LOG_DEBUG(fmt, ...) LOG_DEBUG("C-L08: " fmt, ##__VA_ARGS__)

#define SD_MAX_CALLBACKS 8
#define SD_REGISTRY_VERSION 1
#define SD_SHM_DEFAULT_SIZE (1024 * 1024)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t service_count;
    uint32_t total_instances;
    uint64_t last_modified;
    uint32_t checksum;
    airy_mtx_t shm_mutex;
} sd_registry_header_t;

#define SD_REGISTRY_MAGIC 0x53445247

typedef struct {
    sd_event_callback_t callback;
    void *user_data;
} sd_callback_entry_t;

typedef struct service_discovery_s {
    sd_config_t config;
    sd_service_entry_t services[SD_MAX_SERVICES];
    uint32_t service_count;
    sd_callback_entry_t callbacks[SD_MAX_CALLBACKS];
    uint32_t callback_count;
    sd_stats_t stats;
    bool running;
    airy_mtx_t mutex;
    uint32_t rr_counter;
    void *shm_handle;
    void *shm_ptr;
    bool is_shm_owner;
    const struct sd_backend *backend;
} sd_internal_t;

/* Backend interface: registry-medium abstraction.
 *  - shm   backend: in-memory registry (default, cross-process shared-memory
 *                   semantics).
 *  - file  backend: writes service registrations as JSON files under
 *                   $AIRY_HOME/state/sd/; list/lookup scan the directory,
 *                   implementing a cross-process persistent registry.
 * refresh(name): refresh the working copy from the medium (file reads the
 *                file; name=NULL does a full scan).
 * commit(name):  persist the working copy to the medium (file writes the
 *                file; name=NULL writes everything).
 * refresh/commit are no-ops for the shm backend (memory is the medium). */
typedef struct sd_backend {
    const char *name; /* "shm" / "file" */
    airy_err_t (*init)(sd_internal_t *sd);
    void (*deinit)(sd_internal_t *sd);

    airy_err_t (*register_service)(sd_internal_t *sd, const char *name, const char *type,
                                   const sd_instance_t *inst, const char *tags, const char *deps);
    airy_err_t (*deregister_service)(sd_internal_t *sd, const char *name, const char *instance_id);
    airy_err_t (*deregister_all)(sd_internal_t *sd, const char *name);
    airy_err_t (*lookup_service)(sd_internal_t *sd, const char *name, sd_service_entry_t *out);
    airy_err_t (*list_services)(sd_internal_t *sd, sd_service_entry_t *out, uint32_t max,
                                uint32_t *count);
    airy_err_t (*refresh)(sd_internal_t *sd, const char *name);
    airy_err_t (*commit)(sd_internal_t *sd, const char *name);
} sd_backend_t;

/* 后端实例（service_discovery_backend_shm.c / service_discovery_backend_file.c） */
extern const sd_backend_t sd_backend_shm;
#ifdef AIRY_HAS_CJSON
extern const sd_backend_t sd_backend_file;
#endif

/* 注册表核心操作（service_discovery.c） */
int32_t find_service_index(sd_internal_t *sd, const char *name);
int32_t find_instance_index(sd_service_entry_t *entry, const char *instance_id);
void notify_event(sd_internal_t *sd, sd_event_type_t event, const char *service_name,
                  const sd_instance_t *instance);
bool is_instance_expired(const sd_instance_t *inst, uint32_t expire_ms);
void expire_stale_instances(sd_internal_t *sd);
airy_err_t sd_registry_add_instance(sd_internal_t *sd, const char *name, const char *type,
                                    const sd_instance_t *inst, const char *tags,
                                    const char *deps);
airy_err_t sd_registry_remove_instance(sd_internal_t *sd, const char *name,
                                       const char *instance_id, sd_instance_t *out_removed);
const sd_backend_t *sd_backend_select(void);

/* 负载均衡策略（service_discovery_lb.c） */
airy_err_t lb_round_robin(sd_internal_t *sd, const sd_service_entry_t *entry,
                          sd_instance_t *result);
airy_err_t lb_weighted(const sd_service_entry_t *entry, sd_instance_t *result);
airy_err_t lb_least_connection(const sd_service_entry_t *entry, sd_instance_t *result);
airy_err_t lb_random(const sd_service_entry_t *entry, sd_instance_t *result);
airy_err_t lb_least_load(const sd_service_entry_t *entry, sd_instance_t *result);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_COMMON_SERVICE_DISCOVERY_INTERNAL_H */
