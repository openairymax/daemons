// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file cache_handlers.c
 * @brief Semantic-cache RPC handlers (mem.cache_*，0.1.5).
 *
 * 语义缓存 handlers：cache_put / cache_get / cache_del / cache_stats。
 * 依赖全局 g_cache 实例（创建失败时降级为 no-op）。
 */

#include "cache_handlers.h"
#include "mem_daemon_ctx.h"

#include "airy_memory.h"
#include "error.h"
#include "daemon_main.h"
#include "cache.h"

/* ── mem.cache_put ───────────────────────────────────────────────────── */

void handle_cache_put(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *text = cJSON_GetObjectItem(params, "text");
    cJSON *response = cJSON_GetObjectItem(params, "response");
    cJSON *model_id = cJSON_GetObjectItem(params, "model_id");
    cJSON *ttl = cJSON_GetObjectItem(params, "ttl");

    if (!g_cache || !text || !cJSON_IsString(text) || !response || !cJSON_IsString(response) ||
        !model_id || !cJSON_IsString(model_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "cache_put 需 text/response/model_id 字符串", id);
        return;
    }

    char *cache_id = NULL, *exact_key = NULL;
    uint64_t ttl_ms = ttl && cJSON_IsNumber(ttl) && ttl->valuedouble >= 0
                          ? (uint64_t)ttl->valuedouble
                          : 0;
    int ret = mem_cache_put(g_cache, text->valuestring, response->valuestring,
                            model_id->valuestring, ttl_ms, &cache_id, &exact_key);
    if (ret != AIRY_SUCCESS || !cache_id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "cache_put 失败", id);
        AIRY_FREE(cache_id);
        AIRY_FREE(exact_key);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "cache_id", cache_id);
    cJSON_AddStringToObject(result, "exact_key", exact_key);
    cJSON_AddBoolToObject(result, "ok", 1);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    AIRY_FREE(cache_id);
    AIRY_FREE(exact_key);
}

/* ── mem.cache_get ───────────────────────────────────────────────────── */

void handle_cache_get(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *text = cJSON_GetObjectItem(params, "text");
    cJSON *model_id = cJSON_GetObjectItem(params, "model_id");
    cJSON *threshold = cJSON_GetObjectItem(params, "threshold");

    if (!g_cache || !text || !cJSON_IsString(text) || !model_id || !cJSON_IsString(model_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "cache_get 需 text/model_id", id);
        return;
    }

    int hit = 0;
    double score = 0.0;
    char *cache_id = NULL, *response = NULL;
    double thr = threshold && cJSON_IsNumber(threshold) ? threshold->valuedouble : 0.0;
    int ret = mem_cache_get(g_cache, text->valuestring, model_id->valuestring, thr,
                            &hit, &score, &cache_id, &response);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "cache_get 失败", id);
        AIRY_FREE(cache_id);
        AIRY_FREE(response);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "hit", hit ? 1 : 0);
    cJSON_AddBoolToObject(result, "cache_hit", hit ? 1 : 0);
    cJSON_AddNumberToObject(result, "score", score);
    if (hit && response)
        cJSON_AddStringToObject(result, "response", response);
    if (hit && cache_id)
        cJSON_AddStringToObject(result, "cache_id", cache_id);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    AIRY_FREE(cache_id);
    AIRY_FREE(response);
}

/* ── mem.cache_del ───────────────────────────────────────────────────── */

void handle_cache_del(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *cache_id = cJSON_GetObjectItem(params, "cache_id");
    if (!g_cache || !cache_id || !cJSON_IsString(cache_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "cache_del 需 cache_id", id);
        return;
    }

    int deleted = 0;
    mem_cache_del(g_cache, cache_id->valuestring, &deleted);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "deleted", deleted ? 1 : 0);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ── mem.cache_stats ─────────────────────────────────────────────────── */

void handle_cache_stats(int id, airy_sock_t client_fd)
{
    if (!g_cache) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "cache 未初始化", id);
        return;
    }
    mem_cache_stats_t st;
    mem_cache_stats(g_cache, &st);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "entries", (double)st.entries);
    cJSON_AddNumberToObject(result, "hits", (double)st.hits);
    cJSON_AddNumberToObject(result, "misses", (double)st.misses);
    cJSON_AddNumberToObject(result, "hit_rate", st.hit_rate);
    cJSON_AddNumberToObject(result, "evictions", (double)st.evictions);
    cJSON_AddNumberToObject(result, "bytes", (double)st.bytes);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}
