// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_config.c
 * @brief 服务配置文件加载与监视（config 域）
 *
 * 实现 svc_common.h 中定义的配置接口：加载服务配置文件（json/yaml/toml
 * 依次尝试）、注册/取消配置变更监视、释放配置资源。本域维护独立的
 * g_config_mgr 全局状态，不依赖 service 生命周期域的任何内部结构。
 *
 * 设计原则：
 * 1. 懒初始化（config_mgr_init），首次调用时创建互斥锁
 * 2. 配置文件缺失时返回空配置而非报错，保证服务可无配置启动
 * 3. 单一全局互斥锁保护监视器表，保证线程安全（E-5 并发安全）
 *
 * @see agentrt/daemons/common/include/svc_common.h
 * @see agentrt/daemons/common/src/svc_common.c
 */

#include "svc_common.h"

#include "airy_memory.h"
#include "safe_string_utils.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CONFIG_WATCHERS 32
#define MAX_CONFIG_PATH_LEN 512

typedef struct {
    char service_name[64];
    airy_config_change_callback_t callback;
    void *user_data;
    bool active;
} config_watcher_t;

static struct {
    config_watcher_t watchers[MAX_CONFIG_WATCHERS];
    uint32_t watcher_count;
    char config_base_path[MAX_CONFIG_PATH_LEN];
    bool initialized;
    airy_mtx_t mutex;
} g_config_mgr = {0};

static airy_err_t config_mgr_init(void)
{
    if (g_config_mgr.initialized) {
        return AIRY_SUCCESS;
    }

    airy_err_t err = airy_mtx_init(&g_config_mgr.mutex);
    if (err != AIRY_SUCCESS) {
        return err;
    }

    __builtin_memset(g_config_mgr.watchers, 0, sizeof(g_config_mgr.watchers));
    g_config_mgr.watcher_count = 0;
    if (safe_strcpy(g_config_mgr.config_base_path, "./config",
                    sizeof(g_config_mgr.config_base_path)) != 0) {
        airy_mtx_destroy(&g_config_mgr.mutex);
        return AIRY_EINVAL;
    }
    g_config_mgr.initialized = true;

    return AIRY_SUCCESS;
}

static void compute_simple_checksum(const char *data, size_t len, char *out, size_t out_size)
{
    uint64_t hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)data[i];
    }
    snprintf(out, out_size, "%016llx", (unsigned long long)hash);
}

airy_err_t airy_config_load(const char *service_name, airy_config_t **config)
{
    if (!service_name || !config) {
        return AIRY_EINVAL;
    }

    config_mgr_init();

    char config_path[MAX_CONFIG_PATH_LEN];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(config_path, sizeof(config_path), "%s/%s.json", g_config_mgr.config_base_path,
             service_name);

    FILE *fp = fopen(config_path, "rb");
    if (!fp) {
        snprintf(config_path, sizeof(config_path), "%s/%s.yaml", g_config_mgr.config_base_path,
                 service_name);
        fp = fopen(config_path, "rb");
    }
    if (!fp) {
        snprintf(config_path, sizeof(config_path), "%s/%s.toml", g_config_mgr.config_base_path,
                 service_name);
        fp = fopen(config_path, "rb");
    }
#pragma GCC diagnostic pop

    airy_config_t *cfg = (airy_config_t *)AIRY_CALLOC(1, sizeof(airy_config_t));
    if (!cfg) {
        return AIRY_ENOMEM;
    }

    if (!fp) {
        cfg->raw_config = AIRY_CALLOC(1, 1);
        cfg->config_size = 0;
        cfg->version = 1;
        cfg->last_modified = time(NULL);
        cfg->checksum[0] = '\0';
        *config = cfg;
        LOG_WARN("No config file found for service '%s', using empty config", service_name);
        return AIRY_SUCCESS;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(fp);
        cfg->raw_config = AIRY_CALLOC(1, 1);
        cfg->config_size = 0;
        cfg->version = 1;
        cfg->last_modified = time(NULL);
        *config = cfg;
        return AIRY_SUCCESS;
    }

    cfg->raw_config = (char *)AIRY_CALLOC(1, file_size + 1);
    if (!cfg->raw_config) {
        fclose(fp);
        AIRY_FREE(cfg);
        return AIRY_ENOMEM;
    }

    size_t bytes_read = fread(cfg->raw_config, 1, file_size, fp);
    fclose(fp);
    if (bytes_read != (size_t)file_size) {
        AIRY_FREE(cfg->raw_config);
        AIRY_FREE(cfg);
        return AIRY_EIO;
    }

    cfg->config_size = bytes_read;
    cfg->raw_config[bytes_read] = '\0';
    cfg->version = 1;
    cfg->last_modified = time(NULL);
    compute_simple_checksum(cfg->raw_config, bytes_read, cfg->checksum, sizeof(cfg->checksum));

    *config = cfg;

    LOG_INFO("Config loaded for service '%s': %zu bytes from %s", service_name, bytes_read,
             config_path);
    return AIRY_SUCCESS;
}

airy_err_t airy_config_watch(const char *service_name, airy_config_change_callback_t callback,
                             void *user_data)
{
    if (!service_name || !callback) {
        return AIRY_EINVAL;
    }

    config_mgr_init();

    airy_mtx_lock(&g_config_mgr.mutex);

    if (g_config_mgr.watcher_count >= MAX_CONFIG_WATCHERS) {
        airy_mtx_unlock(&g_config_mgr.mutex);
        return AIRY_ENOMEM;
    }

    for (uint32_t i = 0; i < g_config_mgr.watcher_count; i++) {
        if (g_config_mgr.watchers[i].active &&
            strcmp(g_config_mgr.watchers[i].service_name, service_name) == 0 &&
            g_config_mgr.watchers[i].callback == callback) {
            g_config_mgr.watchers[i].user_data = user_data;
            airy_mtx_unlock(&g_config_mgr.mutex);
            return AIRY_SUCCESS;
        }
    }

    config_watcher_t *watcher = &g_config_mgr.watchers[g_config_mgr.watcher_count];
    if (safe_strcpy(watcher->service_name, service_name, sizeof(watcher->service_name)) != 0) {
        airy_mtx_unlock(&g_config_mgr.mutex);
        return AIRY_EINVAL;
    }
    watcher->callback = callback;
    watcher->user_data = user_data;
    watcher->active = true;
    g_config_mgr.watcher_count++;

    airy_mtx_unlock(&g_config_mgr.mutex);

    LOG_INFO("Config watcher registered for service '%s'", service_name);
    return AIRY_SUCCESS;
}

airy_err_t airy_config_unwatch(const char *service_name, airy_config_change_callback_t callback)
{
    if (!service_name) {
        return AIRY_EINVAL;
    }

    if (!g_config_mgr.initialized) {
        return AIRY_ENOTINIT;
    }

    airy_mtx_lock(&g_config_mgr.mutex);

    for (uint32_t i = 0; i < g_config_mgr.watcher_count; i++) {
        if (g_config_mgr.watchers[i].active &&
            strcmp(g_config_mgr.watchers[i].service_name, service_name) == 0) {
            if (callback == NULL || g_config_mgr.watchers[i].callback == callback) {
                g_config_mgr.watchers[i].active = false;

                if (i < g_config_mgr.watcher_count - 1) {
                    g_config_mgr.watchers[i] =
                        g_config_mgr.watchers[g_config_mgr.watcher_count - 1];
                }
                g_config_mgr.watcher_count--;
                if (callback == NULL) {
                    continue;
                }
            }
        }
    }

    airy_mtx_unlock(&g_config_mgr.mutex);
    return AIRY_SUCCESS;
}

void airy_config_free(airy_config_t *config)
{
    if (!config) {
        return;
    }

    if (config->raw_config) {
        AIRY_FREE(config->raw_config);
        config->raw_config = NULL;
    }

    AIRY_FREE(config);
}
