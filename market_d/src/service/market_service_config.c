// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file market_service_config.c
 * @brief Market service config domain: config hot-reload and remote-registry
 *        sync (downloads the index and incrementally registers new agents).
 */

#include "airy_memory.h"
#include "error.h"
#include "platform.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif

#include "market_service_internal.h"

static void collect_ids(const cJSON *node, char ids[][128], int cap, int *count)
{
    if (!node || *count >= cap)
        return;

    if (cJSON_IsObject(node)) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(node, "agent_id");
        if (cJSON_IsString(id) && id->valuestring && id->valuestring[0] &&
            strlen(id->valuestring) < 128) {
            AIRY_STRNCPY_TERM(ids[*count], id->valuestring, 128);
            (*count)++;
        }
    }

    for (const cJSON *child = node->child; child && *count < cap; child = child->next)
        collect_ids(child, ids, cap, count);
}

/* 下载索引到 out：0 成功；1 下载失败（已记 WARN，调用方按无害路径返回）；
 * 负值为 fork 等致命错误（已入错误栈）。 */
static int sync_download(const char *url, const char *out)
{
#ifdef _WIN32
    const char *const args[] = {"-sfL", "-o", out, url, "--connect-timeout", "10",
                                "--max-time", "60", NULL};
    int ret = win_run_command("curl", args);
#else
    pid_t pid = fork();
    if (pid == 0) {
        execlp("curl", "curl", "-sfL", "-o", out, url, "--connect-timeout", "10",
               "--max-time", "60", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        SVC_LOG_WARN("Sync registry: fork failed: %s", strerror(errno));
        AIRY_ERROR(AIRY_ERR_IO, "fork failed during sync");
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    if (ret != 0) {
        SVC_LOG_WARN("Sync registry: download failed from %s (curl_ret=%d)", url, ret);
        return 1;
    }
    return 0;
}

/* 读入并解析索引：软失败（缺失/尺寸异常/JSON 非法）返回 1 且已记 WARN，
 * 内存或读失败返回负错误码。 */
static int sync_load_index(const char *path, cJSON **root)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        SVC_LOG_WARN("Sync registry: cannot open downloaded index %s", path);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 10 * 1024 * 1024) {
        fclose(fp);
        SVC_LOG_WARN("Sync registry: invalid index size %ld", fsize);
        return 1;
    }

    char *data = (char *)AIRY_MALLOC((size_t)fsize + 1);
    if (!data) {
        fclose(fp);
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "failed to allocate index buffer");
    }
    size_t nread = fread(data, 1, (size_t)fsize, fp);
    fclose(fp);
    if (nread != (size_t)fsize) {
        AIRY_FREE(data);
        AIRY_ERROR(AIRY_ERR_IO, "fread index file failed");
    }
    data[nread] = '\0';

    *root = cJSON_Parse(data);
    AIRY_FREE(data);
    if (!*root) {
        SVC_LOG_WARN("Sync registry: index is not valid JSON");
        return 1;
    }
    return 0;
}

/* 持锁增量注册：跳过已存在的 id，容量上限内新增 AVAILABLE 条目，返回新增数。 */
static int sync_register(market_service_t *service, char ids[][128], int count)
{
    int synced = 0;
    airy_mtx_lock(&service->lock);
    for (int k = 0; k < count; k++) {
        int exists = 0;
        for (size_t i = 0; i < service->agent_count && !exists; i++)
            exists = strcmp(service->agents[i]->agent_id, ids[k]) == 0;
        if (exists || service->agent_count >= AIRY_CAP_MAX_AGENTS)
            continue;

        agent_info_t *agent = (agent_info_t *)AIRY_CALLOC(1, sizeof(agent_info_t));
        if (!agent)
            continue;
        agent->agent_id = AIRY_STRDUP(ids[k]);
        agent->name = AIRY_STRDUP(ids[k]);
        agent->version = AIRY_STRDUP("latest");
        agent->status = AGENT_STATUS_AVAILABLE;
        service->agents[service->agent_count++] = agent;
        synced++;
    }
    airy_mtx_unlock(&service->lock);
    return synced;
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
    const char *cfg_url = service->config.registry_url;
    if (!cfg_url || !cfg_url[0]) {
        SVC_LOG_WARN("Sync registry: no registry_url configured");
        airy_mtx_unlock(&service->lock);
        return 0;
    }
    if (!is_safe_for_shell(cfg_url)) {
        SVC_LOG_WARN("Sync registry: invalid registry_url, possible injection detected");
        airy_mtx_unlock(&service->lock);
        return 0;
    }

    char *snap_url = AIRY_STRDUP(cfg_url);
    char *snap_storage =
        service->config.storage_path ? AIRY_STRDUP(service->config.storage_path) : NULL;
    airy_mtx_unlock(&service->lock);

    int rc = 0;
    if (!snap_url) {
        SVC_LOG_ERROR("Sync registry: strdup failed for registry_url");
        rc = AIRY_ERR_OUT_OF_MEMORY;
        goto out;
    }

    const char *storage = snap_storage ? snap_storage : AIRY_CACHE_DIR;
    char index_path[1024];
    char url[2048];
    (void)airy_mkdir_p(storage);
    snprintf(index_path, sizeof(index_path), "%s/registry_index.json", storage);
    if (strncmp(snap_url, "http", 4) == 0)
        snprintf(url, sizeof(url), "%s/index.json", snap_url);
    else
        snprintf(url, sizeof(url), "https://%s/index.json", snap_url);

    cJSON *root = NULL;
    rc = sync_download(url, index_path);
    if (rc == 0)
        rc = sync_load_index(index_path, &root);
    if (rc == 0) {
        char ids[256][128];
        int n = 0;
        collect_ids(root, ids, 256, &n);
        cJSON_Delete(root);
        SVC_LOG_INFO("Sync registry: synced %d new agents from %s",
                     sync_register(service, ids, n), url);
    } else if (rc > 0) {
        rc = 0;
    }

out:
    AIRY_FREE(snap_url);
    AIRY_FREE(snap_storage);
    return rc;
}
