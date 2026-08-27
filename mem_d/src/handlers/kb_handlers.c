// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file kb_handlers.c
 * @brief Knowledge-base RPC handlers: kb_ingest, kb_search, kb_delete, kb_list.
 *
 * 2.1.2.3 RAG 知识库一等抽象。开发者通过 kb.* 命名空间把文档摄入、
 * 在库内检索、整库删除与列出。
 */

#include "kb_handlers.h"
#include "mem_daemon_ctx.h"

#include "airy_memory.h"
#include "error.h"
#include "daemon_main.h"
#include "mem_service.h"
#include "svc_logger.h"

#include <string.h>

/* ── mem.kb_ingest ───────────────────────────────────────────────────── */

void handle_kb_ingest(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *kb_id = cJSON_GetObjectItem(params, "kb_id");
    cJSON *doc_id = cJSON_GetObjectItem(params, "doc_id");
    cJSON *text = cJSON_GetObjectItem(params, "text");
    cJSON *chunk_size = cJSON_GetObjectItem(params, "chunk_size");

    if (!cJSON_IsString(kb_id) || !kb_id->valuestring || !kb_id->valuestring[0] ||
        !cJSON_IsString(text) || !text->valuestring) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing kb_id/text (kb_id string + text string)", id);
        return;
    }

    size_t chunk = 0;
    if (cJSON_IsNumber(chunk_size))
        chunk = (size_t)chunk_size->valuedouble;

    size_t count = 0;
    int ret = mem_service_kb_ingest(g_service, kb_id->valuestring,
                                    cJSON_IsString(doc_id) ? doc_id->valuestring : "",
                                    text->valuestring, strlen(text->valuestring), chunk, &count);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "KB ingest failed", id);
        SVC_LOG_ERROR("mem.kb_ingest failed: kb=%s error=%d", kb_id->valuestring, ret);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "kb_id", kb_id->valuestring);
    if (cJSON_IsString(doc_id))
        cJSON_AddStringToObject(result, "doc_id", doc_id->valuestring);
    cJSON_AddNumberToObject(result, "chunks", (double)count);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    SVC_LOG_INFO("mem.kb_ingest: kb=%s chunks=%zu", kb_id->valuestring, count);
}

/* ── mem.kb_search ───────────────────────────────────────────────────── */

void handle_kb_search(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *kb_id = cJSON_GetObjectItem(params, "kb_id");
    cJSON *query = cJSON_GetObjectItem(params, "query");
    cJSON *limit = cJSON_GetObjectItem(params, "limit");

    if (!cJSON_IsString(kb_id) || !kb_id->valuestring || !kb_id->valuestring[0] ||
        !cJSON_IsString(query) || !query->valuestring) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing kb_id/query (kb_id string + query string)", id);
        return;
    }

    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    uint32_t lim = limit && cJSON_IsNumber(limit) ? clamp_u32(limit->valueint, 10, 100) : 10;

    int ret = mem_service_kb_search(g_service, kb_id->valuestring, query->valuestring, lim, &hits,
                                    &count);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "KB search failed", id);
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

/* ── mem.kb_delete ───────────────────────────────────────────────────── */

void handle_kb_delete(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *kb_id = cJSON_GetObjectItem(params, "kb_id");
    if (!cJSON_IsString(kb_id) || !kb_id->valuestring || !kb_id->valuestring[0]) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing kb_id string", id);
        return;
    }

    size_t deleted = 0;
    int ret = mem_service_kb_delete(g_service, kb_id->valuestring, &deleted);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "KB delete failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "kb_id", kb_id->valuestring);
    cJSON_AddNumberToObject(result, "deleted_records", (double)deleted);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ── mem.kb_list ─────────────────────────────────────────────────────── */

void handle_kb_list(cJSON *params, int id, airy_sock_t client_fd)
{
    char **kb_ids = NULL;
    size_t count = 0;
    int ret = mem_service_kb_list(g_service, &kb_ids, &count);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "KB list failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "kb_id", kb_ids[i]);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(result, "knowledge_bases", arr);
    cJSON_AddNumberToObject(result, "total", (double)count);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    mem_kb_list_free(kb_ids, count);
}
