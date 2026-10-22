// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file mem_handlers.c
 * @brief Core memory RPC handlers: write, search, get, delete, count,
 *        recent, evolve, health_check, get_stats.
 *
 * Extracted from the monolithic main.c (1380 lines) to keep each handler
 * domain in its own translation unit.  All handlers share the global
 * service/cache/ledger instances declared in mem_daemon_ctx.h.
 */

#include "mem_handlers.h"
#include "core/mem_daemon_ctx.h"

#include "cache_handlers.h"
#include "ledger_handlers.h"

#include "airy_memory.h"
#include "error.h"
#include "daemon_main.h"
#include "core/mem_service.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── mem.write ───────────────────────────────────────────────────────── */

void handle_write(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *data = cJSON_GetObjectItem(params, "data");
    cJSON *metadata = cJSON_GetObjectItem(params, "metadata");

    JSONRPC_REQUIRE(data && cJSON_IsString(data), client_fd, "Missing data string", id);

    mem_write_request_t req = {
        .data = data->valuestring,
        .len = strlen(data->valuestring),
        .metadata = metadata ? cJSON_PrintUnformatted(metadata) : NULL,
    };

    char *out_record_id = NULL;
    int ret = mem_service_write(g_service, &req, &out_record_id);

    if (req.metadata)
        AIRY_FREE((void *)req.metadata);

    if (ret != AIRY_SUCCESS || !out_record_id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Memory write failed", id);
        SVC_LOG_ERROR("mem.write failed: error=%d", ret);
        AIRY_FREE(out_record_id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "record_id", out_record_id);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    AIRY_FREE(out_record_id);
}

/* ── mem.search ──────────────────────────────────────────────────────── */

void handle_search(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *query = cJSON_GetObjectItem(params, "query");
    cJSON *limit = cJSON_GetObjectItem(params, "limit");

    JSONRPC_REQUIRE(query && cJSON_IsString(query), client_fd, "Missing query string", id);

    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    uint32_t lim = limit && cJSON_IsNumber(limit) ? clamp_u32(limit->valueint, 10, 100) : 10;

    int ret = mem_service_search(g_service, query->valuestring, lim, &hits, &count);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Memory search failed", id);
        return;
    }

    cJSON *result = search_hits_json(hits, count);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    mem_search_hits_free(hits, count);
}

/* ── mem.get ─────────────────────────────────────────────────────────── */

void handle_get(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *record_id = cJSON_GetObjectItem(params, "record_id");
    JSONRPC_REQUIRE(record_id && cJSON_IsString(record_id), client_fd, "Missing record_id", id);

    mem_record_t rec = {0};
    int ret = mem_service_get(g_service, record_id->valuestring, &rec);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Record not found", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "data", (const char *)rec.data);
    cJSON_AddNumberToObject(result, "length", (double)rec.len);
    if (rec.metadata)
        cJSON_AddStringToObject(result, "metadata", rec.metadata);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    mem_record_free(&rec);
}

/* ── mem.delete ──────────────────────────────────────────────────────── */

void handle_delete(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *record_id = cJSON_GetObjectItem(params, "record_id");
    JSONRPC_REQUIRE(record_id && cJSON_IsString(record_id), client_fd, "Missing record_id", id);

    int ret = mem_service_delete(g_service, record_id->valuestring);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Record not found", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "deleted", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ── mem.count ───────────────────────────────────────────────────────── */

void handle_count(int id, airy_sock_t client_fd)
{
    size_t n = mem_service_count(g_service);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "count", (double)n);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ── mem.recent ──────────────────────────────────────────────────────── */

/* 供 CLI/TUI 记忆链展示面板使用：每条含完整内容/metadata/created_at。 */
void handle_recent(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *limit = params ? cJSON_GetObjectItem(params, "limit") : NULL;
    uint32_t lim = limit && cJSON_IsNumber(limit) ? clamp_u32(limit->valueint, 0, 1000) : 0;

    mem_recent_item_t *items = NULL;
    size_t count = 0;
    int ret = mem_service_recent(g_service, lim, &items, &count);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Memory recent failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "record_id", items[i].record_id);
        cJSON_AddNumberToObject(item, "created_at", (double)items[i].created_at);
        cJSON_AddNumberToObject(item, "len", (double)items[i].len);
        cJSON_AddStringToObject(item, "data", items[i].data ? (const char *)items[i].data : "");
        if (items[i].metadata)
            cJSON_AddStringToObject(item, "metadata", items[i].metadata);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(result, "records", arr);
    cJSON_AddNumberToObject(result, "total", (double)count);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    mem_recent_items_free(items, count);
}

/* ── mem.evolve ──────────────────────────────────────────────────────── */

/*
 * 合并命中正文并登记来源：逐条取回记录，无论取回成败都登记来源项，
 * 使调用方可追溯"演化自哪些记录"。合并正文为空时 *out_merged 保持 NULL。
 */
static int hits_merge(const mem_search_hit_t *hits, size_t count, cJSON *sources,
                      char **out_merged, size_t *out_len)
{
    char **parts;
    size_t total = 0;
    size_t off = 0;

    *out_merged = NULL;
    *out_len = 0;
    parts = (char **)AIRY_CALLOC(count ? count : 1, sizeof(char *));
    if (!parts)
        return AIRY_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < count; i++) {
        mem_record_t rec = {0};
        cJSON *src;

        if (mem_service_get(g_service, hits[i].record_id, &rec) == AIRY_SUCCESS) {
            char *copy = AIRY_MALLOC(rec.len + 1);
            if (copy) {
                AIRY_MEMCPY(copy, rec.data, rec.len);
                copy[rec.len] = '\0';
                parts[i] = copy;
                total += rec.len;
            }
            mem_record_free(&rec);
        }
        src = cJSON_CreateObject();
        cJSON_AddStringToObject(src, "record_id", hits[i].record_id);
        cJSON_AddNumberToObject(src, "score", (double)hits[i].score);
        cJSON_AddItemToArray(sources, src);
    }

    if (total > 0) {
        *out_merged = AIRY_MALLOC(total + 1);
        if (!*out_merged) {
            for (size_t i = 0; i < count; i++)
                AIRY_FREE(parts[i]);
            AIRY_FREE(parts);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < count; i++) {
            if (parts[i]) {
                size_t len = strlen(parts[i]);
                AIRY_MEMCPY(*out_merged + off, parts[i], len);
                off += len;
            }
            AIRY_FREE(parts[i]);
        }
        (*out_merged)[off] = '\0';
        *out_len = off;
    }
    AIRY_FREE(parts);
    return AIRY_SUCCESS;
}

/* 统一演化应答：status / source_count，可选 evolved_record_id。 */
static void evolve_reply(int id, airy_sock_t fd, const char *status, size_t count,
                         const char *new_id)
{
    cJSON *result = cJSON_CreateObject();

    if (!result) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(result, "status", status);
    if (new_id)
        cJSON_AddStringToObject(result, "evolved_record_id", new_id);
    cJSON_AddNumberToObject(result, "source_count", (double)count);
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

/* query 分支：检索命中 → 合并正文 → 回写一条强化记忆。 */
static void evolve_query(const char *query, cJSON *params, int id, airy_sock_t fd)
{
    cJSON *limit_json = cJSON_GetObjectItem(params, "limit");
    uint32_t lim = limit_json && cJSON_IsNumber(limit_json)
                       ? clamp_u32(limit_json->valueint, 10, 100)
                       : 10;
    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    char *merged = NULL;
    size_t merged_len = 0;
    char *meta_str;
    char *new_id = NULL;
    cJSON *meta;
    cJSON *sources;
    int wret;

    if (mem_service_search(g_service, query, lim, &hits, &count) != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Memory search failed", id);
        return;
    }

    meta = cJSON_CreateObject();
    sources = cJSON_CreateArray();
    if (!meta || !sources) {
        cJSON_Delete(meta);
        cJSON_Delete(sources);
        mem_search_hits_free(hits, count);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(meta, "evolved_from_query", query);
    cJSON_AddNumberToObject(meta, "evolved_at", (double)(uint64_t)time(NULL) * 1000);
    cJSON_AddItemToObject(meta, "sources", sources);

    if (hits_merge(hits, count, sources, &merged, &merged_len) != AIRY_SUCCESS) {
        cJSON_Delete(meta);
        mem_search_hits_free(hits, count);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    if (!merged) {
        cJSON_Delete(meta);
        mem_search_hits_free(hits, count);
        evolve_reply(id, fd, "no_records", count, NULL);
        return;
    }

    meta_str = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!meta_str) {
        AIRY_FREE(merged);
        mem_search_hits_free(hits, count);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Metadata serialize failed", id);
        return;
    }

    mem_write_request_t req = {.data = merged, .len = merged_len, .metadata = meta_str};
    wret = mem_service_write(g_service, &req, &new_id);
    AIRY_FREE(meta_str);
    AIRY_FREE(merged);
    mem_search_hits_free(hits, count);

    if (wret != AIRY_SUCCESS || !new_id) {
        AIRY_FREE(new_id);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Evolve write failed", id);
        return;
    }
    evolve_reply(id, fd, "evolved", count, new_id);
    SVC_LOG_INFO("mem.evolve: query='%s' merged=%zu records -> %s", query, count, new_id);
    AIRY_FREE(new_id);
}

/* record_id 分支：读取单条记录，回写带演化元数据的增强副本。 */
static void evolve_record(const char *rid, int id, airy_sock_t fd)
{
    mem_record_t rec = {0};
    char *meta_str;
    char *new_id = NULL;
    cJSON *result;
    cJSON *meta;
    int wret;

    if (mem_service_get(g_service, rid, &rec) != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_METHOD_NOT_FOUND, "Record not found", id);
        return;
    }

    meta = cJSON_CreateObject();
    if (!meta) {
        mem_record_free(&rec);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(meta, "evolved_from", rid);
    cJSON_AddNumberToObject(meta, "evolved_at", (double)(uint64_t)time(NULL) * 1000);
    cJSON_AddNumberToObject(meta, "access_count", 1);

    meta_str = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!meta_str) {
        mem_record_free(&rec);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Metadata serialize failed", id);
        return;
    }

    mem_write_request_t req = {.data = rec.data, .len = rec.len, .metadata = meta_str};
    wret = mem_service_write(g_service, &req, &new_id);
    AIRY_FREE(meta_str);
    mem_record_free(&rec);

    if (wret != AIRY_SUCCESS || !new_id) {
        AIRY_FREE(new_id);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Evolve write failed", id);
        return;
    }

    result = cJSON_CreateObject();
    if (!result) {
        AIRY_FREE(new_id);
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    cJSON_AddStringToObject(result, "status", "evolved");
    cJSON_AddStringToObject(result, "evolved_record_id", new_id);
    cJSON_AddStringToObject(result, "source_record_id", rid);
    JSONRPC_SEND_SUCCESS(fd, result, id);
    SVC_LOG_INFO("mem.evolve: record %s -> %s", rid, new_id);
    AIRY_FREE(new_id);
}

/*
 * L2 standard method mem.evolve (02-l2-service-protocol.md):
 * real "memory evolution" behavior built on the existing retrieval/write
 * APIs —
 *   - params.query (recommended): retrieve related records and merge the hit
 *     content into one reinforced memory record, with metadata recording the
 *     source record_id, relevance score and evolution time (marking the access
 *     source);
 *   - params.record_id: read a single record and write back an enhanced copy
 *     carrying evolve metadata.
 * The service layer has no standalone evolve function, so the daemon layer
 * composes it from the existing mem_service_search/get/write.
 */
void handle_evolve(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *query = cJSON_GetObjectItem(params, "query");
    cJSON *rid = cJSON_GetObjectItem(params, "record_id");

    if (cJSON_IsString(query) && query->valuestring)
        evolve_query(query->valuestring, params, id, client_fd);
    else if (cJSON_IsString(rid) && rid->valuestring)
        evolve_record(rid->valuestring, id, client_fd);
    else
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing query or record_id", id);
}

/* ── mem.health_check ────────────────────────────────────────────────── */

void handle_health_check(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "mem_d");
    cJSON_AddBoolToObject(result, "healthy", g_service != NULL);
    cJSON_AddNumberToObject(result, "record_count", (double)mem_service_count(g_service));
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ── mem.get_stats ───────────────────────────────────────────────────── */

/* 统计口径唯一来源：语义缓存与台账子对象一并返回，使 /stats mem 与
 * mem.cache_stats / mem.ledger_stats 三路口径一致。 */
cJSON *mem_stats_json(void)
{
    cJSON *result = cJSON_CreateObject();
    if (!result)
        return NULL;
    cJSON_AddStringToObject(result, "daemon", "mem_d");
    cJSON_AddNumberToObject(result, "records",
                            (double)(g_service ? mem_service_count(g_service) : 0));
    cJSON_AddNumberToObject(result, "max_records", (double)g_config.max_records);

    cJSON *cache = mem_cache_stats_json();
    if (cache)
        cJSON_AddItemToObject(result, "cache", cache);
    cJSON *ledger = mem_ledger_stats_json();
    if (ledger)
        cJSON_AddItemToObject(result, "ledger", ledger);

    return result;
}

void handle_get_stats(int id, airy_sock_t client_fd)
{
    cJSON *result = mem_stats_json();
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}
