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
    cJSON *cacheable = cJSON_GetObjectItem(params, "cacheable");

    if (!g_cache || !text || !cJSON_IsString(text) || !response || !cJSON_IsString(response) ||
        !model_id || !cJSON_IsString(model_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "cache_put 需 text/response/model_id 字符串", id);
        return;
    }

    /* B5-3 准入门禁（fail-closed）：未显式声明 cacheable，或文本命中
     * 用户敏感面（凭据 / 私有路径 / 密钥前缀），均拒绝写入。 */
    int declared_cacheable = cJSON_IsBool(cacheable) && cJSON_IsTrue(cacheable);
    if (!declared_cacheable || !mem_cache_admit(text->valuestring)) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", 0);
        cJSON_AddBoolToObject(result, "cached", 0);
        cJSON_AddStringToObject(result, "reason",
                                declared_cacheable ? "sensitive_text" : "not_cacheable");
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
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

/* 命中率口径唯一来源：mem.cache_stats 与 mem.get_stats 均由此构造。 */
cJSON *mem_cache_stats_json(void)
{
    if (!g_cache)
        return NULL;
    mem_cache_stats_t st;
    mem_cache_stats(g_cache, &st);
    cJSON *result = cJSON_CreateObject();
    if (!result)
        return NULL;
    cJSON_AddNumberToObject(result, "entries", (double)st.entries);
    cJSON_AddNumberToObject(result, "hits", (double)st.hits);
    cJSON_AddNumberToObject(result, "misses", (double)st.misses);
    /* hit_rate 为负表示无查询样本：显式标注不可用，不得伪零（§2.1-6） */
    cJSON_AddNumberToObject(result, "hit_rate", st.hit_rate);
    cJSON_AddBoolToObject(result, "hit_rate_available", st.hit_rate >= 0.0 ? 1 : 0);
    cJSON_AddNumberToObject(result, "evictions", (double)st.evictions);
    cJSON_AddNumberToObject(result, "bytes", (double)st.bytes);
    return result;
}

void handle_cache_stats(int id, airy_sock_t client_fd)
{
    cJSON *result = mem_cache_stats_json();
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "cache 未初始化", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}
