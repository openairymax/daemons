// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"

/**
 * @file agent_registry.c
 * @brief Agent 注册表实现（基于实际market_service.h API）
 */

#include "daemon_errors.h"
#include "market_service.h"
#include "daemon_platform_ext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <airymax/sched.h>

typedef struct {
    agent_info_t info;
} agent_entry_t;

typedef struct {
    agent_entry_t entries[AIRY_CAP_MAX_AGENTS];
    size_t entry_count;
    airy_mtx_t lock;
    int initialized;
} agent_registry_t;

static agent_registry_t g_registry = {0};

static int find_agent_index(const char *agent_id)
{
    for (size_t i = 0; i < g_registry.entry_count; i++) {
        if (g_registry.entries[i].info.agent_id &&
            strcmp(g_registry.entries[i].info.agent_id, agent_id) == 0)
            return (int)i;
    }
    return AIRY_ERR_NOT_FOUND;
}

static void free_agent_info(agent_info_t *info)
{
    if (!info)
        return;
    AIRY_FREE(info->agent_id);
    AIRY_FREE(info->name);
    AIRY_FREE(info->version);
    AIRY_FREE(info->description);
    AIRY_FREE(info->author);
    AIRY_FREE(info->repository);
    AIRY_FREE(info->dependencies);
    __builtin_memset(info, 0, sizeof(agent_info_t));
}

static void free_agent_entry(agent_entry_t *entry)
{
    if (!entry)
        return;
    free_agent_info(&entry->info);
}

static char *safe_strdup(const char *str)
{
    if (!str) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    size_t len = strlen(str);
    char *copy = (char *)AIRY_MALLOC(len + 1);
    if (copy)
        __builtin_memcpy(copy, str, len + 1);
    return copy;
}

int agent_registry_init(const char *db_path __attribute__((unused)))
{
    if (g_registry.initialized)
        return AIRY_OK;
    airy_mtx_init(&g_registry.lock);
    g_registry.entry_count = 0;
    g_registry.initialized = 1;
    return AIRY_OK;
}

void agent_registry_shutdown(void)
{
    if (!g_registry.initialized)
        return;
    airy_mtx_lock(&g_registry.lock);
    for (size_t i = 0; i < g_registry.entry_count; i++)
        free_agent_entry(&g_registry.entries[i]);
    g_registry.entry_count = 0;
    g_registry.initialized = 0;
    airy_mtx_unlock(&g_registry.lock);
    airy_mtx_destroy(&g_registry.lock);
}

int agent_registry_register(const agent_info_t *reg)
{
    if (!reg || !reg->agent_id || !reg->name)
        return AIRY_ERR_INVALID_PARAM;
    if (!g_registry.initialized)
        return AIRY_ERR_STATE_ERROR;

    airy_mtx_lock(&g_registry.lock);
    if (find_agent_index(reg->agent_id) >= 0) {
        airy_mtx_unlock(&g_registry.lock);
        return AIRY_ERR_ALREADY_EXISTS;
    }
    if (g_registry.entry_count >= AIRY_CAP_MAX_AGENTS) {
        airy_mtx_unlock(&g_registry.lock);
        return AIRY_ERR_OVERFLOW;
    }

    agent_entry_t *entry = &g_registry.entries[g_registry.entry_count++];
    entry->info.agent_id = safe_strdup(reg->agent_id);
    entry->info.name = safe_strdup(reg->name);
    entry->info.version = safe_strdup(reg->version);
    entry->info.description = safe_strdup(reg->description);
    entry->info.type = reg->type;
    entry->info.status = AGENT_STATUS_AVAILABLE;
    entry->info.author = safe_strdup(reg->author);
    entry->info.repository = safe_strdup(reg->repository);
    entry->info.dependencies = safe_strdup(reg->dependencies);
    entry->info.rating = reg->rating;
    entry->info.download_count = reg->download_count;
    entry->info.last_updated = (uint64_t)time(NULL);

    airy_mtx_unlock(&g_registry.lock);
    return AIRY_OK;
}

int agent_registry_unregister(const char *agent_id)
{
    if (!agent_id)
        return AIRY_ERR_INVALID_PARAM;
    if (!g_registry.initialized)
        return AIRY_ERR_STATE_ERROR;

    airy_mtx_lock(&g_registry.lock);
    int idx = find_agent_index(agent_id);
    if (idx < 0) {
        airy_mtx_unlock(&g_registry.lock);
        return AIRY_ERR_NOT_FOUND;
    }

    free_agent_entry(&g_registry.entries[idx]);
    for (size_t i = (size_t)idx; i < g_registry.entry_count - 1; i++)
        g_registry.entries[i] = g_registry.entries[i + 1];
    __builtin_memset(&g_registry.entries[--g_registry.entry_count], 0, sizeof(agent_entry_t));

    airy_mtx_unlock(&g_registry.lock);
    return AIRY_OK;
}

int agent_registry_get(const char *agent_id, agent_info_t *out_info)
{
    if (!agent_id || !out_info)
        return AIRY_ERR_INVALID_PARAM;
    if (!g_registry.initialized)
        return AIRY_ERR_STATE_ERROR;

    airy_mtx_lock(&g_registry.lock);
    int idx = find_agent_index(agent_id);
    if (idx < 0) {
        airy_mtx_unlock(&g_registry.lock);
        return AIRY_ERR_NOT_FOUND;
    }

    agent_entry_t *entry = &g_registry.entries[idx];
    __builtin_memset(out_info, 0, sizeof(agent_info_t));
    out_info->agent_id = safe_strdup(entry->info.agent_id);
    out_info->name = safe_strdup(entry->info.name);
    out_info->version = safe_strdup(entry->info.version);
    out_info->description = safe_strdup(entry->info.description);
    out_info->type = entry->info.type;
    out_info->status = entry->info.status;
    out_info->author = safe_strdup(entry->info.author);
    out_info->repository = safe_strdup(entry->info.repository);
    out_info->dependencies = safe_strdup(entry->info.dependencies);
    out_info->rating = entry->info.rating;
    out_info->download_count = entry->info.download_count;
    out_info->last_updated = entry->info.last_updated;

    airy_mtx_unlock(&g_registry.lock);
    return AIRY_OK;
}

/* P2-6 修复：agent_registry_get 的不加锁版本，供已持锁的内部函数调用。
 * 避免嵌套加锁（agent_registry_search 持锁时调用 agent_registry_get 会再次获取同一锁）。
 * 调用方必须已持有 g_registry.lock。 */
static int agent_registry_get_locked(const char *agent_id, agent_info_t *out_info)
{
    if (!agent_id || !out_info)
        return AIRY_ERR_INVALID_PARAM;

    int idx = find_agent_index(agent_id);
    if (idx < 0)
        return AIRY_ERR_NOT_FOUND;

    agent_entry_t *entry = &g_registry.entries[idx];
    __builtin_memset(out_info, 0, sizeof(agent_info_t));
    out_info->agent_id = safe_strdup(entry->info.agent_id);
    out_info->name = safe_strdup(entry->info.name);
    out_info->version = safe_strdup(entry->info.version);
    out_info->description = safe_strdup(entry->info.description);
    out_info->type = entry->info.type;
    out_info->status = entry->info.status;
    out_info->author = safe_strdup(entry->info.author);
    out_info->repository = safe_strdup(entry->info.repository);
    out_info->dependencies = safe_strdup(entry->info.dependencies);
    out_info->rating = entry->info.rating;
    out_info->download_count = entry->info.download_count;
    out_info->last_updated = entry->info.last_updated;

    return AIRY_OK;
}

int agent_registry_search(const search_params_t *params, agent_info_t ***results, size_t *count)
{
    if (!params || !results || !count)
        return AIRY_ERR_INVALID_PARAM;
    *results = NULL;
    *count = 0;
    if (!g_registry.initialized)
        return AIRY_ERR_STATE_ERROR;

    const char *query = params->query ? params->query : "";

    airy_mtx_lock(&g_registry.lock);
    size_t match_count = 0;
    for (size_t i = 0; i < g_registry.entry_count; i++) {
        agent_entry_t *entry = &g_registry.entries[i];
        if (!query[0] || (entry->info.agent_id && strstr(entry->info.agent_id, query)) ||
            (entry->info.name && strstr(entry->info.name, query)) ||
            (entry->info.description && strstr(entry->info.description, query))) {
            match_count++;
        }
    }

    if (match_count == 0) {
        airy_mtx_unlock(&g_registry.lock);
        return AIRY_OK;
    }

    *results = (agent_info_t **)AIRY_CALLOC(match_count, sizeof(agent_info_t *));
    if (!*results) {
        airy_mtx_unlock(&g_registry.lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t result_idx = 0;
    for (size_t i = 0; i < g_registry.entry_count && result_idx < match_count; i++) {
        agent_entry_t *entry = &g_registry.entries[i];
        if (!query[0] || (entry->info.agent_id && strstr(entry->info.agent_id, query)) ||
            (entry->info.name && strstr(entry->info.name, query)) ||
            (entry->info.description && strstr(entry->info.description, query))) {

            (*results)[result_idx] = (agent_info_t *)AIRY_CALLOC(1, sizeof(agent_info_t));
            if ((*results)[result_idx]) {
                /* P2-6 修复：调用 _locked 版本避免嵌套加锁。
                 * 当前线程已持有 g_registry.lock，若调用 agent_registry_get 会再次
                 * 尝试获取同一锁（当前依赖 PTHREAD_MUTEX_RECURSIVE 才不致死锁）。 */
                agent_registry_get_locked(entry->info.agent_id, (*results)[result_idx]);
                result_idx++;
            }
        }
    }

    *count = result_idx;
    airy_mtx_unlock(&g_registry.lock);
    return AIRY_OK;
}

int agent_registry_add_version(const char *agent_id, const char *version_str)
{
    if (!agent_id || !version_str)
        return AIRY_ERR_INVALID_PARAM;
    if (!g_registry.initialized)
        return AIRY_ERR_STATE_ERROR;

    airy_mtx_lock(&g_registry.lock);
    int idx = find_agent_index(agent_id);
    if (idx < 0) {
        airy_mtx_unlock(&g_registry.lock);
        return AIRY_ERR_NOT_FOUND;
    }

    agent_entry_t *entry = &g_registry.entries[idx];
    AIRY_FREE(entry->info.version);
    entry->info.version = safe_strdup(version_str);
    entry->info.last_updated = (uint64_t)time(NULL);

    airy_mtx_unlock(&g_registry.lock);
    return AIRY_OK;
}

void agent_info_free(agent_info_t *info)
{
    if (!info)
        return;
    free_agent_info(info);
}

void agent_search_results_free(agent_info_t **results, size_t count)
{
    if (!results)
        return;
    for (size_t i = 0; i < count; i++) {
        if (results[i]) {
            agent_info_free(results[i]);
            AIRY_FREE(results[i]);
        }
    }
    AIRY_FREE(results);
}
