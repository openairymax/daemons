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
#include "mem_daemon_ctx.h"

#include "airy_memory.h"
#include "error.h"
#include "daemon_main.h"
#include "mem_service.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── mem.write ───────────────────────────────────────────────────────── */

void handle_write(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *data = cJSON_GetObjectItem(params, "data");
    cJSON *metadata = cJSON_GetObjectItem(params, "metadata");

    if (!data || !cJSON_IsString(data)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing data string", id);
        return;
    }

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

    if (!query || !cJSON_IsString(query)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing query string", id);
        return;
    }

    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    uint32_t lim = limit && cJSON_IsNumber(limit) ? clamp_u32(limit->valueint, 10, 100) : 10;

    int ret = mem_service_search(g_service, query->valuestring, lim, &hits, &count);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Memory search failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "record_id", hits[i].record_id);
        cJSON_AddNumberToObject(item, "score", hits[i].score);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(result, "results", arr);
    cJSON_AddNumberToObject(result, "total", count);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    mem_search_hits_free(hits, count);
}

/* ── mem.get ─────────────────────────────────────────────────────────── */

void handle_get(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *record_id = cJSON_GetObjectItem(params, "record_id");
    if (!record_id || !cJSON_IsString(record_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing record_id", id);
        return;
    }

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
    if (!record_id || !cJSON_IsString(record_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing record_id", id);
        return;
    }

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

    if (cJSON_IsString(query) && query->valuestring) {
        cJSON *limit_json = cJSON_GetObjectItem(params, "limit");
        uint32_t lim = limit_json && cJSON_IsNumber(limit_json)
                           ? clamp_u32(limit_json->valueint, 10, 100)
                           : 10;

        mem_search_hit_t *hits = NULL;
        size_t count = 0;
        int ret = mem_service_search(g_service, query->valuestring, lim, &hits, &count);
        if (ret != AIRY_SUCCESS) {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Memory search failed", id);
            return;
        }

        cJSON *meta = cJSON_CreateObject();
        cJSON *sources = cJSON_CreateArray();
        if (!meta || !sources) {
            if (meta) cJSON_Delete(meta);
            if (sources) cJSON_Delete(sources);
            mem_search_hits_free(hits, count);
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
            return;
        }
        cJSON_AddStringToObject(meta, "evolved_from_query", query->valuestring);
        cJSON_AddNumberToObject(meta, "evolved_at", (double)(uint64_t)time(NULL) * 1000);

        size_t total_len = 0;
        char **data_parts = (char **)AIRY_CALLOC(count ? count : 1, sizeof(char *));
        if (count > 0 && !data_parts) {
            cJSON_Delete(meta);
            cJSON_Delete(sources);
            mem_search_hits_free(hits, count);
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
            return;
        }

        for (size_t i = 0; i < count; i++) {
            mem_record_t rec = {0};
            if (mem_service_get(g_service, hits[i].record_id, &rec) == AIRY_SUCCESS) {
                size_t dlen = rec.len;
                char *copy = AIRY_MALLOC(dlen + 1);
                if (copy) {
                    AIRY_MEMCPY(copy, rec.data, dlen);
                    copy[dlen] = '\0';
                    data_parts[i] = copy;
                    total_len += dlen;
                }
                mem_record_free(&rec);
                cJSON *src = cJSON_CreateObject();
                cJSON_AddStringToObject(src, "record_id", hits[i].record_id);
                cJSON_AddNumberToObject(src, "score", (double)hits[i].score);
                cJSON_AddItemToArray(sources, src);
            } else {
                cJSON *src = cJSON_CreateObject();
                cJSON_AddStringToObject(src, "record_id", hits[i].record_id);
                cJSON_AddNumberToObject(src, "score", (double)hits[i].score);
                cJSON_AddItemToArray(sources, src);
            }
        }
        cJSON_AddItemToObject(meta, "sources", sources);

        char *merged = NULL;
        if (total_len > 0) {
            merged = (char *)AIRY_MALLOC(total_len + 1);
            if (!merged) {
                for (size_t i = 0; i < count; i++)
                    AIRY_FREE(data_parts[i]);
                AIRY_FREE(data_parts);
                cJSON_Delete(meta);
                mem_search_hits_free(hits, count);
                JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
                return;
            }
            size_t off = 0;
            for (size_t i = 0; i < count; i++) {
                if (data_parts[i]) {
                    size_t len = strlen(data_parts[i]);
                    __builtin_memcpy(merged + off, data_parts[i], len);
                    off += len;
                }
                AIRY_FREE(data_parts[i]);
            }
            merged[off] = '\0';
        }
        AIRY_FREE(data_parts);

        char *meta_str = cJSON_PrintUnformatted(meta);
        cJSON_Delete(meta);
        if (!meta_str) {
            AIRY_FREE(merged);
            mem_search_hits_free(hits, count);
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Metadata serialize failed", id);
            return;
        }

        mem_write_request_t req = {.data = merged ? merged : "",
                                   .len = merged ? strlen(merged) : 0,
                                   .metadata = meta_str};
        char *new_id = NULL;
        int wret = mem_service_write(g_service, &req, &new_id);
        AIRY_FREE(meta_str);
        AIRY_FREE(merged);
        mem_search_hits_free(hits, count);

        if (wret != AIRY_SUCCESS || !new_id) {
            AIRY_FREE(new_id);
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Evolve write failed", id);
            return;
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", count > 0 ? "evolved" : "no_records");
        cJSON_AddStringToObject(result, "evolved_record_id", new_id);
        cJSON_AddNumberToObject(result, "source_count", (double)count);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_INFO("mem.evolve: query='%s' merged=%zu records -> %s", query->valuestring, count,
                     new_id);
        AIRY_FREE(new_id);
        return;
    }

    if (cJSON_IsString(rid) && rid->valuestring) {
        mem_record_t rec = {0};
        int ret = mem_service_get(g_service, rid->valuestring, &rec);
        if (ret != AIRY_SUCCESS) {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Record not found", id);
            return;
        }

        cJSON *meta = cJSON_CreateObject();
        if (!meta) {
            mem_record_free(&rec);
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
            return;
        }
        cJSON_AddStringToObject(meta, "evolved_from", rid->valuestring);
        cJSON_AddNumberToObject(meta, "evolved_at", (double)(uint64_t)time(NULL) * 1000);
        cJSON_AddNumberToObject(meta, "access_count", 1);

        char *meta_str = cJSON_PrintUnformatted(meta);
        cJSON_Delete(meta);
        if (!meta_str) {
            mem_record_free(&rec);
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Metadata serialize failed", id);
            return;
        }

        mem_write_request_t req = {.data = rec.data, .len = rec.len, .metadata = meta_str};
        char *new_id = NULL;
        int wret = mem_service_write(g_service, &req, &new_id);
        AIRY_FREE(meta_str);
        mem_record_free(&rec);

        if (wret != AIRY_SUCCESS || !new_id) {
            AIRY_FREE(new_id);
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Evolve write failed", id);
            return;
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "evolved");
        cJSON_AddStringToObject(result, "evolved_record_id", new_id);
        cJSON_AddStringToObject(result, "source_record_id", rid->valuestring);
        JSONRPC_SEND_SUCCESS(client_fd, result, id);
        SVC_LOG_INFO("mem.evolve: record %s -> %s", rid->valuestring, new_id);
        AIRY_FREE(new_id);
        return;
    }

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

void handle_get_stats(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "daemon", "mem_d");
    if (g_service) {
        cJSON_AddNumberToObject(result, "records", (double)mem_service_count(g_service));
    } else {
        cJSON_AddNumberToObject(result, "records", 0);
    }
    cJSON_AddNumberToObject(result, "max_records", (double)g_config.max_records);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}
