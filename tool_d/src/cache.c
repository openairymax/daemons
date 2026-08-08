// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#include "airy_memory.h"
#include "error.h"
/**
 * @file cache.c
 * @brief 工具结果缓存实现（LRU?
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "cache.h"
#include "memory_common.h"
#include "daemon_platform_ext.h"
#include "tool_service.h"

#include <cjson/cJSON.h>
/* P0.18.2: 引入 cjson_helpers.h 提供 CJSON_PARSE_GUARD/CJSON_AUTO_FREE 宏 */
#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HASH_SIZE 1024

typedef struct cache_entry {
    char *key;
    char *value;
    time_t timestamp;
    struct cache_entry *prev;
    struct cache_entry *next;
    struct cache_entry *hnext;
} cache_entry_t;

typedef struct cache_bucket {
    cache_entry_t *head;
    airy_mtx_t lock;
} cache_bucket_t;

struct tool_cache {
    cache_bucket_t buckets[HASH_SIZE];
    cache_entry_t *lru_head;
    cache_entry_t *lru_tail;
    size_t capacity;
    size_t size;
    int ttl_sec;
    airy_mtx_t lru_lock;
};

static unsigned int hash_key(const char *key)
{
    unsigned int h = 5381;
    while (*key)
        h = (h << 5) + h + *key++;
    return h % HASH_SIZE;
}

static cache_entry_t *entry_create(const char *key, const char *value)
{
    cache_entry_t *e = memory_safe_alloc(sizeof(cache_entry_t));
    if (!e) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    e->key = memory_safe_strdup(key);
    e->value = memory_safe_strdup(value);
    if (!e->key || !e->value) {
        memory_safe_free(e->key);
        memory_safe_free(e->value);
        memory_safe_free(e);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    e->timestamp = time(NULL);
    e->prev = e->next = e->hnext = NULL;
    return e;
}

static void entry_memory_safe_free(cache_entry_t *e)
{
    if (!e)
        return;
    memory_safe_free(e->key);
    memory_safe_free(e->value);
    memory_safe_free(e);
}

static void lru_remove(tool_cache_t *cache, cache_entry_t *e)
{
    if (e->prev)
        e->prev->next = e->next;
    if (e->next)
        e->next->prev = e->prev;
    if (cache->lru_head == e)
        cache->lru_head = e->next;
    if (cache->lru_tail == e)
        cache->lru_tail = e->prev;
    e->prev = e->next = NULL;
}

static void lru_move_to_head(tool_cache_t *cache, cache_entry_t *e)
{
    if (cache->lru_head == e)
        return;
    lru_remove(cache, e);
    e->next = cache->lru_head;
    if (cache->lru_head)
        cache->lru_head->prev = e;
    cache->lru_head = e;
    if (!cache->lru_tail)
        cache->lru_tail = e;
}

static void evict_lru(tool_cache_t *cache)
{
    airy_mtx_lock(&cache->lru_lock);
    if (!cache->lru_tail) {
        airy_mtx_unlock(&cache->lru_lock);
        return;
    }
    cache_entry_t *victim = cache->lru_tail;
    unsigned int idx = hash_key(victim->key);

    airy_mtx_lock(&cache->buckets[idx].lock);
    cache_entry_t **p = &cache->buckets[idx].head;
    while (*p) {
        if (*p == victim) {
            *p = victim->hnext;
            break;
        }
        p = &(*p)->hnext;
    }
    airy_mtx_unlock(&cache->buckets[idx].lock);

    lru_remove(cache, victim);
    entry_memory_safe_free(victim);
    cache->size--;
    airy_mtx_unlock(&cache->lru_lock);
}

tool_cache_t *tool_cache_create(size_t capacity, int ttl_sec)
{
    tool_cache_t *cache = AIRY_CALLOC(1, sizeof(tool_cache_t));
    if (!cache) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    cache->capacity = capacity;
    cache->ttl_sec = ttl_sec;
    airy_mtx_init(&cache->lru_lock);
    for (int i = 0; i < HASH_SIZE; ++i)
        airy_mtx_init(&cache->buckets[i].lock);
    return cache;
}

void tool_cache_destroy(tool_cache_t *cache)
{
    if (!cache)
        return;
    for (int i = 0; i < HASH_SIZE; ++i) {
        airy_mtx_lock(&cache->buckets[i].lock);
        cache_entry_t *e = cache->buckets[i].head;
        while (e) {
            cache_entry_t *next = e->hnext;
            entry_memory_safe_free(e);
            e = next;
        }
        airy_mtx_unlock(&cache->buckets[i].lock);
        airy_mtx_destroy(&cache->buckets[i].lock);
    }
    airy_mtx_destroy(&cache->lru_lock);
    memory_safe_free(cache);
}

int tool_cache_get(tool_cache_t *cache, const char *key, char **out_value)
{
    if (!cache || !key || !out_value)
        return AIRY_ERR_INVALID_PARAM;
    *out_value = NULL;

    unsigned int idx = hash_key(key);
    airy_mtx_lock(&cache->buckets[idx].lock);
    cache_entry_t *e = cache->buckets[idx].head;
    while (e) {
        if (strcmp(e->key, key) == 0)
            break;
        e = e->hnext;
    }
    airy_mtx_unlock(&cache->buckets[idx].lock);

    if (!e)
        return 0;

    if (cache->ttl_sec > 0 && (time(NULL) - e->timestamp) > cache->ttl_sec) {
        tool_cache_put(cache, key, NULL);
        return 0;
    }

    airy_mtx_lock(&cache->lru_lock);
    lru_move_to_head(cache, e);
    airy_mtx_unlock(&cache->lru_lock);

    *out_value = memory_safe_strdup(e->value);
    return 1;
}

void tool_cache_put(tool_cache_t *cache, const char *key, const char *value)
{
    if (!cache || !key)
        return;
    if (cache->capacity == 0)
        return;

    unsigned int idx = hash_key(key);
    airy_mtx_lock(&cache->buckets[idx].lock);

    cache_entry_t **p = &cache->buckets[idx].head;
    while (*p) {
        if (strcmp((*p)->key, key) == 0) {
            cache_entry_t *e = *p;
            *p = e->hnext;
            airy_mtx_unlock(&cache->buckets[idx].lock);

            airy_mtx_lock(&cache->lru_lock);
            lru_remove(cache, e);
            cache->size--;
            airy_mtx_unlock(&cache->lru_lock);

            entry_memory_safe_free(e);
            airy_mtx_lock(&cache->buckets[idx].lock);
            break;
        }
        p = &(*p)->hnext;
    }

    if (!value) {
        airy_mtx_unlock(&cache->buckets[idx].lock);
        return;
    }

    cache_entry_t *e = entry_create(key, value);
    if (!e) {
        airy_mtx_unlock(&cache->buckets[idx].lock);
        return;
    }

    e->hnext = cache->buckets[idx].head;
    cache->buckets[idx].head = e;
    airy_mtx_unlock(&cache->buckets[idx].lock);

    airy_mtx_lock(&cache->lru_lock);
    e->next = cache->lru_head;
    if (cache->lru_head)
        cache->lru_head->prev = e;
    cache->lru_head = e;
    if (!cache->lru_tail)
        cache->lru_tail = e;
    cache->size++;
    airy_mtx_unlock(&cache->lru_lock);

    if (cache->size > cache->capacity) {
        evict_lru(cache);
    }
}

void tool_cache_clear(tool_cache_t *cache)
{
    if (!cache)
        return;

    for (int i = 0; i < HASH_SIZE; ++i) {
        airy_mtx_lock(&cache->buckets[i].lock);
        cache_entry_t *e = cache->buckets[i].head;
        while (e) {
            cache_entry_t *next = e->hnext;
            entry_memory_safe_free(e);
            e = next;
        }
        cache->buckets[i].head = NULL;
        airy_mtx_unlock(&cache->buckets[i].lock);
    }

    airy_mtx_lock(&cache->lru_lock);
    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    cache->size = 0;
    airy_mtx_unlock(&cache->lru_lock);
}

char *tool_cache_key(const char *tool_id, const char *params_json, const char *agent_id)
{
    if (!tool_id || !params_json) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    /* 缓存键纳入 agent_id（SEC-xx：防止未授权 agent 命中他人审批放行的缓存，
     * 绕过权限审批）。agent_id 为空时退化为 "tool_d|tool|params" 兼容既有行为。 */
    const char *subject = (agent_id && agent_id[0]) ? agent_id : "tool_d";

    size_t tool_id_len = strlen(tool_id);
    size_t params_len = strlen(params_json);
    size_t subject_len = strlen(subject);
    size_t len = subject_len + tool_id_len + params_len + 3;

    char *key = memory_safe_alloc(len);
    if (!key) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    snprintf(key, len, "%s|%s|%s", subject, tool_id, params_json);
    return key;
}

tool_result_t *tool_result_from_json(const char *json)
{
    /* P0.18.2: 模式 A — CJSON_PARSE_GUARD 自动释放 + NULL 检查 */
    CJSON_PARSE_GUARD(root, json, {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    });
    tool_result_t *res = AIRY_CALLOC(1, sizeof(tool_result_t));
    if (!res) {
        /* root 由 CJSON_AUTO_FREE 自动释放 */
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    cJSON *success = cJSON_GetObjectItem(root, "success");
    if (cJSON_IsNumber(success))
        res->success = success->valueint;
    cJSON *output = cJSON_GetObjectItem(root, "output");
    if (cJSON_IsString(output))
        res->output = memory_safe_strdup(output->valuestring);
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsString(error))
        res->error = memory_safe_strdup(error->valuestring);
    cJSON *exit_code = cJSON_GetObjectItem(root, "exit_code");
    if (cJSON_IsNumber(exit_code))
        res->exit_code = exit_code->valueint;
    /* root 由 CJSON_AUTO_FREE 自动释放 */
    return res;
}

char *tool_result_to_json(const tool_result_t *res)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "success", res->success);
    if (res->output)
        cJSON_AddStringToObject(root, "output", res->output);
    if (res->error)
        cJSON_AddStringToObject(root, "error", res->error);
    cJSON_AddNumberToObject(root, "exit_code", res->exit_code);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
