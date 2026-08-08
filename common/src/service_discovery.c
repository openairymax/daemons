// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
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

/* C-L08: ServiceDiscovery 日志前缀 */
#define SD_LOG_INFO(fmt, ...)  LOG_INFO("C-L08: " fmt, ##__VA_ARGS__)
#define SD_LOG_WARN(fmt, ...)  LOG_WARN("C-L08: " fmt, ##__VA_ARGS__)
#define SD_LOG_ERROR(fmt, ...) LOG_ERROR("C-L08: " fmt, ##__VA_ARGS__)
#define SD_LOG_DEBUG(fmt, ...) LOG_DEBUG("C-L08: " fmt, ##__VA_ARGS__)

/* ==================== 内部常量 ==================== */

#define SD_MAX_CALLBACKS 8
#define SD_REGISTRY_VERSION 1
#define SD_SHM_DEFAULT_SIZE (1024 * 1024)

/* ==================== 共享内存注册表头 ==================== */

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

/* ==================== 内部数据结构 ==================== */

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
    const struct sd_backend *backend; /**< 当前注册中心后端（select 于 create，BORROW） */
} sd_internal_t;

/* 后端接口：注册中心介质抽象。
 *  - shm  后端：内存注册表（默认，跨进程共享内存语义）。
 *  - file 后端：把服务注册写为 JSON 文件到 $AIRY_HOME/state/sd/，
 *               list/lookup 扫描目录，实现跨进程持久化注册中心。
 * refresh(name)：从介质刷新工作副本（file 读文件；name=NULL 全量扫描）。
 * commit(name)： 将工作副本持久化到介质（file 写文件；name=NULL 全量写入）。
 * refresh/commit 对 shm 后端为 no-op（内存即介质）。 */
typedef struct sd_backend {
    const char *name; /* "shm" / "file" */
    airy_err_t (*init)(sd_internal_t *sd);
    void (*deinit)(sd_internal_t *sd);
    /* 注册中心核心操作 */
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

/* ==================== 辅助函数 ==================== */

static int32_t find_service_index(sd_internal_t *sd, const char *name)
{
    for (uint32_t i = 0; i < sd->service_count; i++) {
        if (strcmp(sd->services[i].name, name) == 0)
            return (int32_t)i;
    }
    /* "未找到"是正常控制流（调用者通过返回值判断），不是错误。
     * 之前调用 AIRY_ERROR_HANDLE 会在每次查找未命中时分配 error context，
     * 导致内存泄漏（尤其在并发注册场景下）。 */
    return AIRY_ERR_NOT_FOUND;
}

static int32_t find_instance_index(sd_service_entry_t *entry, const char *instance_id)
{
    for (uint32_t i = 0; i < entry->instance_count; i++) {
        if (strcmp(entry->instances[i].instance_id, instance_id) == 0)
            return (int32_t)i;
    }
    /* 同 find_service_index：正常控制流，不分配 error context */
    return AIRY_ERR_NOT_FOUND;
}

static void notify_event(sd_internal_t *sd, sd_event_type_t event, const char *service_name,
                         const sd_instance_t *instance)
{
    for (uint32_t i = 0; i < sd->callback_count; i++) {
        if (sd->callbacks[i].callback) {
            sd->callbacks[i].callback(event, service_name, instance, sd->callbacks[i].user_data);
        }
    }
}

static bool is_instance_expired(const sd_instance_t *inst, uint32_t expire_ms)
{
    if (expire_ms == 0)
        return false;
    uint64_t now = airy_time_ms();
    return (now - inst->last_heartbeat) > expire_ms;
}

static void expire_stale_instances(sd_internal_t *sd)
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
                         (unsigned long long)(now - expired.last_heartbeat),
                         sd->service_count, entry->instance_count);

                if (j < entry->instance_count - 1) {
                    entry->instances[j] = entry->instances[entry->instance_count - 1];
                }
                __builtin_memset(&entry->instances[entry->instance_count - 1], 0, sizeof(sd_instance_t));
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
                __builtin_memset(&sd->services[sd->service_count - 1], 0, sizeof(sd_service_entry_t));
                sd->service_count--;
                changed = true;
                i--;
            }
        }
    }
    /* 有实例/服务被移除时，将变更持久化到介质（shm 后端 no-op） */
    if (changed && sd->backend)
        sd->backend->commit(sd, NULL);
}

/* ==================== 后端抽象（多后端注册中心） ==================== */

/* 注册表变更通用逻辑（shm/file 后端共用）：新增或更新服务实例 */
static airy_err_t sd_registry_add_instance(sd_internal_t *sd, const char *name, const char *type,
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
        entry->instances[inst_idx].register_time =
            entry->instances[inst_idx].register_time > 0
                ? entry->instances[inst_idx].register_time
                : airy_time_ms();
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

/* 注册表变更通用逻辑：移除服务实例（out_removed 可为 NULL） */
static airy_err_t sd_registry_remove_instance(sd_internal_t *sd, const char *name,
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

/* ---------- shm 后端：内存注册表（默认） ---------- */

static airy_err_t shm_init(sd_internal_t *sd)
{
    (void)sd;
    return AIRY_SUCCESS;
}

static void shm_deinit(sd_internal_t *sd)
{
    (void)sd;
}

static airy_err_t shm_register(sd_internal_t *sd, const char *name, const char *type,
                               const sd_instance_t *inst, const char *tags, const char *deps)
{
    return sd_registry_add_instance(sd, name, type, inst, tags, deps);
}

static airy_err_t shm_deregister(sd_internal_t *sd, const char *name, const char *instance_id)
{
    return sd_registry_remove_instance(sd, name, instance_id, NULL);
}

static airy_err_t shm_deregister_all(sd_internal_t *sd, const char *name)
{
    int32_t svc_idx = find_service_index(sd, name);
    if (svc_idx < 0)
        return AIRY_ENOENT;
    sd->services[svc_idx].instance_count = 0;
    sd->services[svc_idx].last_updated = airy_time_ms();
    return AIRY_SUCCESS;
}

static airy_err_t shm_lookup(sd_internal_t *sd, const char *name, sd_service_entry_t *out)
{
    int32_t svc_idx = find_service_index(sd, name);
    if (svc_idx < 0)
        return AIRY_ENOENT;
    __builtin_memcpy(out, &sd->services[svc_idx], sizeof(sd_service_entry_t));
    return AIRY_SUCCESS;
}

static airy_err_t shm_list(sd_internal_t *sd, sd_service_entry_t *out, uint32_t max,
                           uint32_t *count)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < sd->service_count && n < max; i++)
        __builtin_memcpy(&out[n++], &sd->services[i], sizeof(sd_service_entry_t));
    *count = n;
    return AIRY_SUCCESS;
}

static airy_err_t shm_refresh(sd_internal_t *sd, const char *name)
{
    (void)sd;
    (void)name;
    return AIRY_SUCCESS;
}

static airy_err_t shm_commit(sd_internal_t *sd, const char *name)
{
    (void)sd;
    (void)name;
    return AIRY_SUCCESS;
}

static const sd_backend_t sd_backend_shm = {
    .name = "shm",
    .init = shm_init,
    .deinit = shm_deinit,
    .register_service = shm_register,
    .deregister_service = shm_deregister,
    .deregister_all = shm_deregister_all,
    .lookup_service = shm_lookup,
    .list_services = shm_list,
    .refresh = shm_refresh,
    .commit = shm_commit,
};

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>

/* ---------- file 后端：JSON 文件注册表（$AIRY_HOME/state/sd/） ---------- */

/* 服务名 → 安全文件名（把路径分隔符等不安全字符替换为 '_'） */
static void sd_safe_filename(const char *name, char *out, size_t out_sz)
{
    size_t i = 0;
    while (*name && i + 1 < out_sz) {
        char c = *name++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') {
            out[i++] = c;
        } else {
            out[i++] = '_';
        }
    }
    out[i] = '\0';
}

/* 目录：$AIRY_HOME/state/sd（无 AIRY_HOME 时回退到相对路径 state/sd） */
static void sd_file_dir(char *buf, size_t size)
{
    const char *home = airy_home_dir();
    if (!home)
        snprintf(buf, size, "state/sd");
    else
        snprintf(buf, size, "%s/state/sd", home);
}

/* 服务文件路径：$AIRY_HOME/state/sd/<name>.json */
static void sd_file_path(const char *name, char *buf, size_t size)
{
    char dir[512], safe[SD_MAX_NAME_LEN];
    sd_file_dir(dir, sizeof(dir));
    sd_safe_filename(name, safe, sizeof(safe));
    snprintf(buf, size, "%s/%s.json", dir, safe);
}

/* 文件名 → 服务名（去掉 .json 后缀；仅用于与注册表内 name 匹配） */
static void sd_file_name_from_path(const char *path, char *out, size_t out_sz)
{
    size_t i = 0;
    const char *p = path;
    while (*p && *p != '.' && i + 1 < out_sz)
        out[i++] = *p++;
    out[i] = '\0';
}

/* 序列化单个服务条目到 JSON 文件（先写临时文件再 rename，避免读到半株文件） */
static airy_err_t sd_file_write_service(const sd_service_entry_t *entry)
{
    char dir[512], path[512];
    sd_file_dir(dir, sizeof(dir));
    if (airy_mkdir_p(dir) != 0)
        return AIRY_ERR_SYS_FILE;
    sd_file_path(entry->name, path, sizeof(path));

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return AIRY_ENOMEM;
    cJSON_AddStringToObject(root, "name", entry->name);
    cJSON_AddStringToObject(root, "version", entry->version);
    cJSON_AddStringToObject(root, "service_type", entry->service_type);
    cJSON_AddStringToObject(root, "tags", entry->tags);
    cJSON_AddStringToObject(root, "dependencies", entry->dependencies);
    cJSON_AddNumberToObject(root, "capabilities", (double)entry->capabilities);
    cJSON_AddBoolToObject(root, "active", entry->active);
    cJSON_AddNumberToObject(root, "last_updated", (double)entry->last_updated);

    cJSON *insts = cJSON_AddArrayToObject(root, "instances");
    for (uint32_t i = 0; i < entry->instance_count; i++) {
        const sd_instance_t *in = &entry->instances[i];
        cJSON *o = cJSON_CreateObject();
        if (!o)
            continue;
        cJSON_AddStringToObject(o, "instance_id", in->instance_id);
        cJSON_AddStringToObject(o, "endpoint", in->endpoint);
        cJSON_AddNumberToObject(o, "state", (double)in->state);
        cJSON_AddBoolToObject(o, "healthy", in->healthy);
        cJSON_AddNumberToObject(o, "weight", (double)in->weight);
        cJSON_AddNumberToObject(o, "active_connections", (double)in->active_connections);
        cJSON_AddNumberToObject(o, "max_connections", (double)in->max_connections);
        cJSON_AddNumberToObject(o, "last_heartbeat", (double)in->last_heartbeat);
        cJSON_AddNumberToObject(o, "register_time", (double)in->register_time);
        cJSON_AddNumberToObject(o, "pid", (double)in->pid);
        cJSON_AddItemToArray(insts, o);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return AIRY_ENOMEM;

    char tmp[544];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        AIRY_FREE(json);
        return AIRY_ERR_SYS_FILE;
    }
    size_t len = strlen(json);
    size_t written = fwrite(json, 1, len, f);
    int rc = fclose(f);
    AIRY_FREE(json);
    if (written != len || rc != 0)
        return AIRY_ERR_SYS_FILE;
    if (rename(tmp, path) != 0)
        return AIRY_ERR_SYS_FILE;
    return AIRY_SUCCESS;
}

/* 从 JSON 文件读取单个服务条目（文件不存在返回 AIRY_ENOENT） */
static airy_err_t sd_file_read_service(const char *name, sd_service_entry_t *out)
{
    char path[512];
    sd_file_path(name, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return AIRY_ENOENT;

    long sz;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return AIRY_ERR_SYS_FILE;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return AIRY_ERR_SYS_FILE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return AIRY_ERR_SYS_FILE;
    }

    char *buf = (char *)AIRY_CALLOC(1, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return AIRY_ENOMEM;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    AIRY_FREE(buf);
    if (!root)
        return AIRY_ERR_PARSE_ERROR;

    __builtin_memset(out, 0, sizeof(*out));
    const char *s;
    cJSON *it;

    s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "name"));
    if (s)
        safe_strcpy(out->name, s, SD_MAX_NAME_LEN);
    s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "version"));
    if (s)
        safe_strcpy(out->version, s, sizeof(out->version));
    s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "service_type"));
    if (s)
        safe_strcpy(out->service_type, s, SD_MAX_TYPE_LEN);
    s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "tags"));
    if (s)
        safe_strcpy(out->tags, s, SD_MAX_TAGS_LEN);
    s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "dependencies"));
    if (s)
        safe_strcpy(out->dependencies, s, SD_MAX_DEPS_LEN);
    it = cJSON_GetObjectItemCaseSensitive(root, "capabilities");
    if (it && cJSON_IsNumber(it))
        out->capabilities = (uint32_t)it->valuedouble;
    it = cJSON_GetObjectItemCaseSensitive(root, "active");
    if (it && cJSON_IsBool(it))
        out->active = cJSON_IsTrue(it);
    it = cJSON_GetObjectItemCaseSensitive(root, "last_updated");
    if (it && cJSON_IsNumber(it))
        out->last_updated = (uint64_t)it->valuedouble;

    cJSON *insts = cJSON_GetObjectItemCaseSensitive(root, "instances");
    if (insts && cJSON_IsArray(insts)) {
        uint32_t idx = 0;
        for (cJSON *o = insts->child; o && idx < SD_MAX_INSTANCES; o = o->next, idx++) {
            sd_instance_t *in = &out->instances[idx];
            s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "instance_id"));
            if (s)
                safe_strcpy(in->instance_id, s, SD_MAX_NAME_LEN);
            s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "endpoint"));
            if (s)
                safe_strcpy(in->endpoint, s, SD_MAX_ENDPOINT_LEN);
            it = cJSON_GetObjectItemCaseSensitive(o, "state");
            if (it && cJSON_IsNumber(it))
                in->state = (airy_svc_state_t)it->valuedouble;
            it = cJSON_GetObjectItemCaseSensitive(o, "healthy");
            if (it && cJSON_IsBool(it))
                in->healthy = cJSON_IsTrue(it);
            it = cJSON_GetObjectItemCaseSensitive(o, "weight");
            if (it && cJSON_IsNumber(it))
                in->weight = (uint32_t)it->valuedouble;
            it = cJSON_GetObjectItemCaseSensitive(o, "active_connections");
            if (it && cJSON_IsNumber(it))
                in->active_connections = (uint32_t)it->valuedouble;
            it = cJSON_GetObjectItemCaseSensitive(o, "max_connections");
            if (it && cJSON_IsNumber(it))
                in->max_connections = (uint32_t)it->valuedouble;
            it = cJSON_GetObjectItemCaseSensitive(o, "last_heartbeat");
            if (it && cJSON_IsNumber(it))
                in->last_heartbeat = (uint64_t)it->valuedouble;
            it = cJSON_GetObjectItemCaseSensitive(o, "register_time");
            if (it && cJSON_IsNumber(it))
                in->register_time = (uint64_t)it->valuedouble;
            it = cJSON_GetObjectItemCaseSensitive(o, "pid");
            if (it && cJSON_IsNumber(it))
                in->pid = (uint32_t)it->valuedouble;
        }
        out->instance_count = idx;
    }
    cJSON_Delete(root);
    return AIRY_SUCCESS;
}

/* file 后端：从介质刷新工作副本（name=NULL 全量扫描目录） */
static airy_err_t file_refresh(sd_internal_t *sd, const char *name)
{
    if (name) {
        sd_service_entry_t entry;
        airy_err_t err = sd_file_read_service(name, &entry);
        if (err != AIRY_SUCCESS)
            return err;
        int32_t idx = find_service_index(sd, name);
        if (idx >= 0) {
            sd->services[idx] = entry;
        } else if (sd->service_count < SD_MAX_SERVICES) {
            sd->services[sd->service_count++] = entry;
        }
        return AIRY_SUCCESS;
    }

    char dir[512];
    sd_file_dir(dir, sizeof(dir));
    DIR *d = opendir(dir);
    if (!d)
        return (errno == ENOENT) ? AIRY_ENOENT : AIRY_ERR_SYS_FILE;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        size_t l = strlen(de->d_name);
        if (l < 6 || strcmp(de->d_name + l - 5, ".json") != 0)
            continue;
        char sname[SD_MAX_NAME_LEN];
        sd_file_name_from_path(de->d_name, sname, sizeof(sname));
        if (sname[0] == '\0')
            continue;
        sd_service_entry_t entry;
        if (sd_file_read_service(sname, &entry) != AIRY_SUCCESS)
            continue;
        int32_t idx = find_service_index(sd, sname);
        if (idx >= 0)
            sd->services[idx] = entry;
        else if (sd->service_count < SD_MAX_SERVICES)
            sd->services[sd->service_count++] = entry;
    }
    closedir(d);
    return AIRY_SUCCESS;
}

/* file 后端：将工作副本持久化到介质（name=NULL 全量写入） */
static airy_err_t file_commit(sd_internal_t *sd, const char *name)
{
    if (name) {
        int32_t idx = find_service_index(sd, name);
        if (idx < 0)
            return AIRY_ENOENT;
        return sd_file_write_service(&sd->services[idx]);
    }
    airy_err_t err = AIRY_SUCCESS;
    for (uint32_t i = 0; i < sd->service_count; i++) {
        airy_err_t e = sd_file_write_service(&sd->services[i]);
        if (e != AIRY_SUCCESS)
            err = e;
    }
    return err;
}

static airy_err_t file_init(sd_internal_t *sd)
{
    char dir[512];
    sd_file_dir(dir, sizeof(dir));
    if (airy_mkdir_p(dir) != 0)
        return AIRY_ERR_SYS_FILE;
    (void)sd;
    return AIRY_SUCCESS;
}

static void file_deinit(sd_internal_t *sd)
{
    (void)sd;
}

static airy_err_t file_register(sd_internal_t *sd, const char *name, const char *type,
                                const sd_instance_t *inst, const char *tags, const char *deps)
{
    airy_err_t err = sd_registry_add_instance(sd, name, type, inst, tags, deps);
    if (err != AIRY_SUCCESS)
        return err;
    int32_t idx = find_service_index(sd, name);
    if (idx < 0)
        return AIRY_ERR_STATE_ERROR;
    return sd_file_write_service(&sd->services[idx]);
}

static airy_err_t file_deregister(sd_internal_t *sd, const char *name, const char *instance_id)
{
    airy_err_t err = sd_registry_remove_instance(sd, name, instance_id, NULL);
    if (err != AIRY_SUCCESS)
        return err;
    int32_t idx = find_service_index(sd, name);
    if (idx < 0)
        return AIRY_SUCCESS;
    if (sd->services[idx].instance_count == 0) {
        /* 服务无剩余实例：删除其文件 */
        char path[512];
        sd_file_path(name, path, sizeof(path));
        remove(path);
        return AIRY_SUCCESS;
    }
    return sd_file_write_service(&sd->services[idx]);
}

static airy_err_t file_deregister_all(sd_internal_t *sd, const char *name)
{
    int32_t svc_idx = find_service_index(sd, name);
    if (svc_idx < 0)
        return AIRY_ENOENT;
    sd->services[svc_idx].instance_count = 0;
    sd->services[svc_idx].last_updated = airy_time_ms();
    char path[512];
    sd_file_path(name, path, sizeof(path));
    remove(path);
    return AIRY_SUCCESS;
}

static airy_err_t file_lookup(sd_internal_t *sd, const char *name, sd_service_entry_t *out)
{
    /* 先扫描介质刷新工作副本，再从中返回条目 */
    file_refresh(sd, name);
    int32_t svc_idx = find_service_index(sd, name);
    if (svc_idx < 0)
        return AIRY_ENOENT;
    __builtin_memcpy(out, &sd->services[svc_idx], sizeof(sd_service_entry_t));
    return AIRY_SUCCESS;
}

static airy_err_t file_list(sd_internal_t *sd, sd_service_entry_t *out, uint32_t max,
                            uint32_t *count)
{
    file_refresh(sd, NULL);
    uint32_t n = 0;
    for (uint32_t i = 0; i < sd->service_count && n < max; i++)
        __builtin_memcpy(&out[n++], &sd->services[i], sizeof(sd_service_entry_t));
    *count = n;
    return AIRY_SUCCESS;
}

static const sd_backend_t sd_backend_file = {
    .name = "file",
    .init = file_init,
    .deinit = file_deinit,
    .register_service = file_register,
    .deregister_service = file_deregister,
    .deregister_all = file_deregister_all,
    .lookup_service = file_lookup,
    .list_services = file_list,
    .refresh = file_refresh,
    .commit = file_commit,
};

#endif /* AIRY_HAS_CJSON */

/* 后端选择：AIRY_SD_BACKEND=file|shm（默认 shm）；无 cJSON 时仅有 shm 可用 */
static const sd_backend_t *sd_backend_select(void)
{
#ifdef AIRY_HAS_CJSON
    const char *env = getenv("AIRY_SD_BACKEND");
    if (env && (strcmp(env, "file") == 0 || strcmp(env, "filesystem") == 0))
        return &sd_backend_file;
#endif
    return &sd_backend_shm;
}

/* ==================== 负载均衡选择 ==================== */

static airy_err_t lb_round_robin(sd_internal_t *sd, const sd_service_entry_t *entry,
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

static airy_err_t lb_weighted(const sd_service_entry_t *entry, sd_instance_t *result)
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

static airy_err_t lb_least_connection(const sd_service_entry_t *entry, sd_instance_t *result)
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

static airy_err_t lb_random(const sd_service_entry_t *entry, sd_instance_t *result)
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

static airy_err_t lb_least_load(const sd_service_entry_t *entry, sd_instance_t *result)
{
    int32_t best_idx = -1;
    uint32_t min_load = UINT32_MAX;

    for (uint32_t i = 0; i < entry->instance_count; i++) {
        if (!entry->instances[i].healthy)
            continue;
        uint32_t load =
            entry->instances[i].max_connections > 0
                ? entry->instances[i].active_connections * 100 / entry->instances[i].max_connections
                : 0;
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

/* ==================== 公共API实现 ==================== */

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

    /* 选择并初始化注册中心后端（AIRY_SD_BACKEND=file|shm，默认 shm） */
    sd->backend = sd_backend_select();
    if (sd->backend->init(sd) != AIRY_SUCCESS) {
        airy_mtx_destroy(&sd->mutex);
        AIRY_FREE(sd);
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "backend init failed");
    }

    SD_LOG_INFO("CREATE (heartbeat=%ums expire=%ums lb=%s backend=%s)",
             sd->config.heartbeat_interval_ms, sd->config.expire_timeout_ms,
             sd_lb_strategy_to_string(sd->config.default_lb_strategy),
             sd->backend->name);
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

    SD_LOG_INFO("START (heartbeat=%ums expire=%ums)",
             sd->config.heartbeat_interval_ms, sd->config.expire_timeout_ms);
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

/* ==================== 服务注册 ==================== */

AIRY_API airy_err_t sd_register(service_discovery_t sd_handle, const char *service_name,
                                        const char *service_type, const sd_instance_t *instance,
                                        const char *tags, const char *dependencies)
{
    if (!sd_handle || !service_name || !service_type || !instance)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);
    airy_err_t err = sd->backend->register_service(sd, service_name, service_type,
                                                   instance, tags, dependencies);
    airy_mtx_unlock(&sd->mutex);

    if (err != AIRY_SUCCESS) {
        if (err == AIRY_ENOMEM)
            SD_LOG_ERROR("REGISTER failed (registry/instance full) service='%s'", service_name);
        return err;
    }

    notify_event(sd, SD_EVENT_REGISTERED, service_name, instance);

    SD_LOG_INFO("REGISTER service='%s' instance='%s' type='%s' "
             "endpoint='%s' (total_svcs=%u total_insts=%u)",
             service_name, instance->instance_id, service_type,
             instance->endpoint, sd->service_count,
             sd->stats.active_instances);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_deregister(service_discovery_t sd_handle, const char *service_name,
                                          const char *instance_id)
{
    if (!sd_handle || !service_name || !instance_id)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);
    /* 先捕获被移除实例，供回调与日志使用 */
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
             service_name, instance_id,
             sd->service_count, sd->stats.active_instances);
    return AIRY_SUCCESS;
}

AIRY_API airy_err_t sd_deregister_all(service_discovery_t sd_handle,
                                              const char *service_name)
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

/* ==================== 服务发现 ==================== */

AIRY_API airy_err_t sd_discover(service_discovery_t sd_handle, const char *service_name,
                                        sd_instance_t *instances, uint32_t max_count,
                                        uint32_t *found_count)
{
    if (!sd_handle || !service_name || !instances || !found_count)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    /* 先刷新目标服务的工作副本（file 后端读文件；shm 后端 no-op） */
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

AIRY_API airy_err_t sd_discover_by_type(service_discovery_t sd_handle,
                                                const char *service_type,
                                                sd_service_entry_t *entries, uint32_t max_count,
                                                uint32_t *found_count)
{
    if (!sd_handle || !service_type || !entries || !found_count)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    /* 全量刷新工作副本（file 后端扫描目录；shm 后端 no-op） */
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

AIRY_API airy_err_t sd_select_instance(service_discovery_t sd_handle,
                                               const char *service_name, sd_lb_strategy_t strategy,
                                               sd_instance_t *instance)
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

/* ==================== 心跳与健康 ==================== */

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

AIRY_API airy_err_t sd_update_health(service_discovery_t sd_handle,
                                             const char *service_name, const char *instance_id,
                                             bool healthy)
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

AIRY_API airy_err_t sd_update_connections(service_discovery_t sd_handle,
                                                  const char *service_name, const char *instance_id,
                                                  uint32_t active_connections)
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

/* ==================== 依赖管理 ==================== */

AIRY_API airy_err_t sd_get_dependencies(service_discovery_t sd_handle,
                                                const char *service_name, char *dependencies,
                                                size_t max_len)
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

AIRY_API airy_err_t sd_check_dependencies(service_discovery_t sd_handle,
                                                  const char *service_name, char *missing_deps,
                                                  size_t max_len)
{
    if (!sd_handle || !service_name)
        return AIRY_EINVAL;

    sd_internal_t *sd = (sd_internal_t *)sd_handle;

    airy_mtx_lock(&sd->mutex);

    /* 依赖检查需跨服务，先全量刷新工作副本 */
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

/* ==================== 事件与统计 ==================== */

AIRY_API airy_err_t sd_register_event_callback(service_discovery_t sd_handle,
                                                       sd_event_callback_t callback,
                                                       void *user_data)
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

AIRY_API bool sd_is_running(service_discovery_t sd_handle)
{
    if (!sd_handle)
        return false;
    sd_internal_t *sd = (sd_internal_t *)sd_handle;
    return sd->running;
}

AIRY_API const char *sd_lb_strategy_to_string(sd_lb_strategy_t strategy)
{
    static const char *strategy_strings[] = {"ROUND_ROBIN", "WEIGHTED", "LEAST_CONNECTION",
                                             "RANDOM", "LEAST_LOAD"};

    if (strategy < 0 || strategy > SD_LB_LEAST_LOAD)
        return "UNKNOWN";
    return strategy_strings[strategy];
}

/* ==================== C-L08: 统计摘要 ==================== */

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

    /* 计算健康实例数 */
    uint32_t healthy_instances = 0;
    for (uint32_t i = 0; i < sd->service_count; i++) {
        for (uint32_t j = 0; j < sd->services[i].instance_count; j++) {
            if (sd->services[i].instances[j].healthy) healthy_instances++;
        }
    }

    airy_mtx_unlock(&sd->mutex);

    SD_LOG_INFO("SD-STATS services=%u instances=%u (%u healthy) "
                "registrations=%llu deregistrations=%llu "
                "discoveries=%llu heartbeats=%llu "
                "expirations=%llu lb_selections=%llu "
                "running=%s",
                stats.active_services, stats.active_instances,
                healthy_instances,
                (unsigned long long)stats.registrations,
                (unsigned long long)stats.deregistrations,
                (unsigned long long)stats.discoveries,
                (unsigned long long)stats.heartbeats,
                (unsigned long long)stats.expirations,
                (unsigned long long)stats.lb_selections,
                sd->running ? "yes" : "no");
}
