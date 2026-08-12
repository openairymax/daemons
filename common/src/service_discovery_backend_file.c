// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_discovery_backend_file.c
 * @brief 跨进程服务发现 file 后端：将服务注册写入 $AIRY_HOME/state/sd/ 下
 *        的 JSON 文件，list/lookup 扫描目录，实现跨进程持久化注册中心
 */

#include "service_discovery_internal.h"

#ifdef AIRY_HAS_CJSON
#include <cjson/cJSON.h>

#if AIRY_PLATFORM_POSIX
#include <dirent.h>
#endif

static void sd_safe_filename(const char *name, char *out, size_t out_sz)
{
    size_t i = 0;
    while (*name && i + 1 < out_sz) {
        char c = *name++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_') {
            out[i++] = c;
        } else {
            out[i++] = '_';
        }
    }
    out[i] = '\0';
}

static void sd_file_dir(char *buf, size_t size)
{
    const char *home = airy_home_dir();
    if (!home)
        snprintf(buf, size, "state/sd");
    else
        snprintf(buf, size, "%s/state/sd", home);
}

static void sd_file_path(const char *name, char *buf, size_t size)
{
    char dir[512], safe[SD_MAX_NAME_LEN];
    sd_file_dir(dir, sizeof(dir));
    sd_safe_filename(name, safe, sizeof(safe));
    snprintf(buf, size, "%s/%s.json", dir, safe);
}

static void sd_file_name_from_path(const char *path, char *out, size_t out_sz)
{
    size_t i = 0;
    const char *p = path;
    while (*p && *p != '.' && i + 1 < out_sz)
        out[i++] = *p++;
    out[i] = '\0';
}

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

const sd_backend_t sd_backend_file = {
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
