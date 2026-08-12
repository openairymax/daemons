// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file market_service_config.c
 * @brief 市场服务配置域：配置热加载与远端注册中心同步（下载索引并
 *        增量注册新 agent）
 */

#include "airy_memory.h"
#include "error.h"
#include "market_service.h"
#include "platform.h"
#include "svc_logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#else
#include <windows.h>
#endif

#include "market_service_internal.h"

int market_service_reload_config(market_service_t *service, const market_config_t *config)
{
    if (!service || !config || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&service->lock);

    // Save old owned pointers
    char *old_url = (char *)service->config.registry_url;
    char *old_path = (char *)service->config.storage_path;

    // Copy non-pointer fields, preserving old owned pointers temporarily
    {
        market_config_t tmp = *config;
        tmp.registry_url = old_url;
        tmp.storage_path = old_path;
        service->config = tmp;
    }

    // Replace pointer fields with our own copies
    char *new_url = config->registry_url ? AIRY_STRDUP(config->registry_url) : NULL;
    char *new_path = config->storage_path ? AIRY_STRDUP(config->storage_path) : NULL;

    if (config->registry_url && !new_url) {
        AIRY_FREE(new_path);
        airy_mtx_unlock(&service->lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate registry_url");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    if (config->storage_path && !new_path) {
        AIRY_FREE(new_url);
        service->config.registry_url = new_url;
        airy_mtx_unlock(&service->lock);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to duplicate storage_path");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    service->config.registry_url = new_url;
    service->config.storage_path = new_path;
    AIRY_FREE(old_url);
    AIRY_FREE(old_path);
    airy_mtx_unlock(&service->lock);

    return 0;
}

int market_service_sync_registry(market_service_t *service)
{
    if (!service || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&service->lock);
    if (!service->config.enable_remote_registry) {
        airy_mtx_unlock(&service->lock);
        return 0;
    }

    if (!service->config.registry_url || strlen(service->config.registry_url) == 0) {
        SVC_LOG_WARN("Sync registry: no registry_url configured");
        airy_mtx_unlock(&service->lock);
        return 0;
    }

    if (!is_safe_for_shell(service->config.registry_url)) {
        SVC_LOG_WARN("Sync registry: invalid registry_url, possible injection detected");
        airy_mtx_unlock(&service->lock);
        return 0;
    }

    char *snap_url = AIRY_STRDUP(service->config.registry_url);
    char *snap_storage =
        service->config.storage_path ? AIRY_STRDUP(service->config.storage_path) : NULL;
    airy_mtx_unlock(&service->lock);

    if (!snap_url) {
        AIRY_FREE(snap_storage);
        SVC_LOG_ERROR("Sync registry: strdup failed for registry_url");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    const char *storage = snap_storage ? snap_storage : AIRY_CACHE_DIR;

    {
        size_t pos = 0;
        char tmp[1024];
        AIRY_STRNCPY_TERM(tmp, storage, sizeof(tmp));
        (tmp)[sizeof(tmp) - 1] = '\0';
        tmp[sizeof(tmp) - 1] = '\0';
        while (tmp[pos]) {
            if (tmp[pos] == '/' && pos > 0) {
                tmp[pos] = '\0';
                mkdir(tmp, 0755);
                tmp[pos] = '/';
            }
            pos++;
        }
        mkdir(tmp, 0755);
    }

    char index_path[1024];
    snprintf(index_path, sizeof(index_path), "%s/registry_index.json", storage);

    char url[2048];
    if (strncmp(snap_url, "http", 4) == 0) {
        snprintf(url, sizeof(url), "%s/index.json", snap_url);
    } else {
        snprintf(url, sizeof(url), "https://%s/index.json", snap_url);
    }

#ifdef _WIN32
    /* Windows equivalent: win_run_command (CreateProcess) replaces
     * fork/execlp/waitpid. Behavior aligned: on curl failure (including
     * CreateProcess not finding curl) warn and return 0. */
    {
        const char *const curl_args[] = {"-sfL", "-o",         index_path, url, "--connect-timeout",
                                         "10",   "--max-time", "60",       NULL};
        int curl_ret = win_run_command("curl", curl_args);
        if (curl_ret != 0) {
            SVC_LOG_WARN("Sync registry: download failed from %s (curl_ret=%d)", url, curl_ret);
            AIRY_FREE(snap_url);
            AIRY_FREE(snap_storage);
            return 0;
        }
    }
#else
    pid_t curl_pid = fork();
    if (curl_pid == 0) {
        execlp("curl", "curl", "-sfL", "-o", index_path, url, "--connect-timeout", "10",
               "--max-time", "60", (char *)NULL);
        _exit(127);
    } else if (curl_pid > 0) {
        int curl_status = 0;
        waitpid(curl_pid, &curl_status, 0);
        int curl_ret = WIFEXITED(curl_status) ? WEXITSTATUS(curl_status) : -1;
        if (curl_ret != 0) {
            SVC_LOG_WARN("Sync registry: download failed from %s (curl_ret=%d)", url, curl_ret);
            AIRY_FREE(snap_url);
            AIRY_FREE(snap_storage);
            return 0;
        }
    } else {
        SVC_LOG_WARN("Sync registry: fork failed: %s", strerror(errno));
        AIRY_FREE(snap_url);
        AIRY_FREE(snap_storage);
        AIRY_ERROR(AIRY_ERR_IO, "fork failed during sync");
    }
#endif

    FILE *idx_fp = fopen(index_path, "r");
    if (!idx_fp) {
        SVC_LOG_WARN("Sync registry: cannot open downloaded index %s", index_path);
        AIRY_FREE(snap_url);
        AIRY_FREE(snap_storage);
        return 0;
    }

    fseek(idx_fp, 0, SEEK_END);
    long fsize = ftell(idx_fp);
    fseek(idx_fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 10 * 1024 * 1024) {
        fclose(idx_fp);
        SVC_LOG_WARN("Sync registry: invalid index size %ld", fsize);
        AIRY_FREE(snap_url);
        AIRY_FREE(snap_storage);
        return 0;
    }

    char *idx_data = (char *)AIRY_MALLOC((size_t)fsize + 1);
    if (!idx_data) {
        fclose(idx_fp);
        AIRY_FREE(snap_url);
        AIRY_FREE(snap_storage);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate index buffer");
    }
    size_t nread = fread(idx_data, 1, (size_t)fsize, idx_fp);
    if (nread != (size_t)fsize) {
        AIRY_FREE(idx_data);
        idx_data = NULL;
        fclose(idx_fp);
        AIRY_FREE(snap_url);
        AIRY_FREE(snap_storage);
        AIRY_ERROR(AIRY_ERR_IO, "fread index file failed");
    }
    idx_data[nread] = '\0';
    fclose(idx_fp);

    char found_ids[256][128];
    int n_found = 0;
    char *entry = strstr(idx_data, "\"agent_id\"");
    while (entry && n_found < 256) {
        char *id_start = strchr(entry, ':');
        if (!id_start)
            break;
        id_start++;
        while (*id_start && (*id_start == ' ' || *id_start == '\t' || *id_start == '"'))
            id_start++;
        char *id_end = id_start;
        while (*id_end && *id_end != '"' && *id_end != ',' && *id_end != '}')
            id_end++;

        size_t id_len = (size_t)(id_end - id_start);
        if (id_len > 0 && id_len < 128) {
            __builtin_memcpy(found_ids[n_found], id_start, id_len);
            found_ids[n_found][id_len] = '\0';
            n_found++;
        }

        entry = strstr(id_end + 1, "\"agent_id\"");
    }
    AIRY_FREE(idx_data);
    idx_data = NULL;

    int synced = 0;
    airy_mtx_lock(&service->lock);
    for (int k = 0; k < n_found; k++) {
        int already_exists = 0;
        for (size_t i = 0; i < service->agent_count; i++) {
            if (strcmp(service->agents[i]->agent_id, found_ids[k]) == 0) {
                already_exists = 1;
                break;
            }
        }

        if (!already_exists && service->agent_count < AIRY_CAP_MAX_AGENTS) {
            agent_info_t *new_agent = (agent_info_t *)AIRY_CALLOC(1, sizeof(agent_info_t));
            if (new_agent) {
                new_agent->agent_id = AIRY_STRDUP(found_ids[k]);
                new_agent->name = AIRY_STRDUP(found_ids[k]);
                new_agent->version = AIRY_STRDUP("latest");
                new_agent->status = AGENT_STATUS_AVAILABLE;
                service->agents[service->agent_count++] = new_agent;
                synced++;
            }
        }
    }
    airy_mtx_unlock(&service->lock);

    AIRY_FREE(snap_url);
    AIRY_FREE(snap_storage);
    SVC_LOG_INFO("Sync registry: synced %d new agents from %s", synced, url);
    return 0;
}
