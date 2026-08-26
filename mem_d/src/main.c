// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file main.c
 * @brief Memory service daemon main entry (daemon module conventions).
 *
 * Exposes JSON-RPC methods (mem.* namespace):
 *   - mem.write   : write a memory record
 *   - mem.search  : keyword search
 *   - mem.get     : read by ID
 *   - mem.delete  : delete by ID
 *   - mem.count   : current record count (health-check helper)
 *
 * Unix socket path: ${AIRY_RUNTIME_DIR}/mem.sock
 */

#include "daemon_main.h"
#include "platform.h"
#include "mem_service.h"
#include "cache.h"
#include "ledger.h"
#include "compress.h"
#include "param_validator.h"
#include "svc_logger.h"
#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("mem.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_mem"
#define DEFAULT_TCP_PORT 8085
#define MAX_BUFFER 65536
#define MAX_CLIENTS 64
#define MEM_DEFAULT_MAX_RECORDS 1024

/* 数值钳制：负数按默认值、超上限按上限（防 RPC 传负值/超大值转无符号回绕） */
static uint32_t clamp_u32(int64_t v, uint32_t def, uint32_t max_v)
{
    if (v < 0)
        return def;
    if ((uint64_t)v > (uint64_t)max_v)
        return max_v;
    return (uint32_t)v;
}

DAEMON_DECLARE_COMMON(mem_d, mem, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(mem_d)

static mem_service_t *g_service = NULL;
/* 语义缓存 + 上下文台账（0.1.5：13-semantic-cache-context-ledger.md 实现） */
static mem_cache_t *g_cache = NULL;
static mem_ledger_t *g_ledger = NULL;

typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_clients;
    size_t max_records;
} mem_daemon_config_t;

static mem_daemon_config_t g_config = {0};

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_mem_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static void handle_write(cJSON *params, int id, airy_sock_t fd);
static void handle_search(cJSON *params, int id, airy_sock_t fd);
static void handle_get(cJSON *params, int id, airy_sock_t fd);
static void handle_delete(cJSON *params, int id, airy_sock_t fd);
static void handle_count(int id, airy_sock_t fd);
static void handle_evolve(cJSON *params, int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);
static void handle_get_stats(int id, airy_sock_t fd);
static void handle_recent(cJSON *params, int id, airy_sock_t fd);
static void handle_kb_ingest(cJSON *params, int id, airy_sock_t fd);
static void handle_kb_search(cJSON *params, int id, airy_sock_t fd);
static void handle_kb_delete(cJSON *params, int id, airy_sock_t fd);
static void handle_kb_list(cJSON *params, int id, airy_sock_t fd);

/* 语义缓存（mem.cache_*，0.1.5） */
static void handle_cache_put(cJSON *params, int id, airy_sock_t fd);
static void handle_cache_get(cJSON *params, int id, airy_sock_t fd);
static void handle_cache_del(cJSON *params, int id, airy_sock_t fd);
static void handle_cache_stats(int id, airy_sock_t fd);

/* 上下文台账（mem.ledger_*，0.1.5） */
static void handle_ledger_append(cJSON *params, int id, airy_sock_t fd);
static void handle_ledger_window(cJSON *params, int id, airy_sock_t fd);
static void handle_ledger_budget(cJSON *params, int id, airy_sock_t fd);
static void handle_ledger_mark(cJSON *params, int id, airy_sock_t fd);
static void handle_ledger_history(cJSON *params, int id, airy_sock_t fd);
static void handle_ledger_stats(int id, airy_sock_t fd);

/* 提示词压缩（mem.compress，0.1.5） */
static void handle_compress(cJSON *params, int id, airy_sock_t fd);

static void on_write_method(cJSON *params, int id, void *user_data)
{
    handle_write(params, id, *(airy_sock_t *)user_data);
}

static void on_search_method(cJSON *params, int id, void *user_data)
{
    handle_search(params, id, *(airy_sock_t *)user_data);
}

static void on_get_method(cJSON *params, int id, void *user_data)
{
    handle_get(params, id, *(airy_sock_t *)user_data);
}

static void on_delete_method(cJSON *params, int id, void *user_data)
{
    handle_delete(params, id, *(airy_sock_t *)user_data);
}

static void on_count_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_count(id, *(airy_sock_t *)user_data);
}

static void on_recent_method(cJSON *params, int id, void *user_data)
{
    handle_recent(params, id, *(airy_sock_t *)user_data);
}

static void on_evolve_method(cJSON *params, int id, void *user_data)
{
    handle_evolve(params, id, *(airy_sock_t *)user_data);
}

static void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
}

static void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

static void on_kb_ingest_method(cJSON *params, int id, void *user_data)
{
    handle_kb_ingest(params, id, *(airy_sock_t *)user_data);
}

static void on_kb_search_method(cJSON *params, int id, void *user_data)
{
    handle_kb_search(params, id, *(airy_sock_t *)user_data);
}

static void on_kb_delete_method(cJSON *params, int id, void *user_data)
{
    handle_kb_delete(params, id, *(airy_sock_t *)user_data);
}

static void on_kb_list_method(cJSON *params, int id, void *user_data)
{
    handle_kb_list(params, id, *(airy_sock_t *)user_data);
}

static void on_cache_put_method(cJSON *params, int id, void *user_data)
{
    handle_cache_put(params, id, *(airy_sock_t *)user_data);
}

static void on_cache_get_method(cJSON *params, int id, void *user_data)
{
    handle_cache_get(params, id, *(airy_sock_t *)user_data);
}

static void on_cache_del_method(cJSON *params, int id, void *user_data)
{
    handle_cache_del(params, id, *(airy_sock_t *)user_data);
}

static void on_cache_stats_method(cJSON *params, int id, void *user_data)
{
    handle_cache_stats(id, *(airy_sock_t *)user_data);
}

static void on_ledger_append_method(cJSON *params, int id, void *user_data)
{
    handle_ledger_append(params, id, *(airy_sock_t *)user_data);
}

static void on_ledger_window_method(cJSON *params, int id, void *user_data)
{
    handle_ledger_window(params, id, *(airy_sock_t *)user_data);
}

static void on_ledger_budget_method(cJSON *params, int id, void *user_data)
{
    handle_ledger_budget(params, id, *(airy_sock_t *)user_data);
}

static void on_ledger_mark_method(cJSON *params, int id, void *user_data)
{
    handle_ledger_mark(params, id, *(airy_sock_t *)user_data);
}

static void on_ledger_history_method(cJSON *params, int id, void *user_data)
{
    handle_ledger_history(params, id, *(airy_sock_t *)user_data);
}

static void on_ledger_stats_method(cJSON *params, int id, void *user_data)
{
    handle_ledger_stats(id, *(airy_sock_t *)user_data);
}

static void on_compress_method(cJSON *params, int id, void *user_data)
{
    handle_compress(params, id, *(airy_sock_t *)user_data);
}

static void handle_write(cJSON *params, int id, airy_sock_t client_fd)
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

static void handle_search(cJSON *params, int id, airy_sock_t client_fd)
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

static void handle_get(cJSON *params, int id, airy_sock_t client_fd)
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

static void handle_delete(cJSON *params, int id, airy_sock_t client_fd)
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

static void handle_count(int id, airy_sock_t client_fd)
{
    size_t n = mem_service_count(g_service);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "count", (double)n);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ---- 记忆链（mem.recent，2026-08-25）：最近写入记录倒序返回 ----
 * 供 CLI/TUI 记忆链展示面板使用：每条含完整内容/metadata/created_at。 */
static void handle_recent(cJSON *params, int id, airy_sock_t client_fd)
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

/* ---- KB 知识库（2.1.2.3 RAG 一等抽象）----
 * mem.kb_ingest / mem.kb_search / mem.kb_delete / mem.kb_list */

static void handle_kb_ingest(cJSON *params, int id, airy_sock_t client_fd)
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

static void handle_kb_search(cJSON *params, int id, airy_sock_t client_fd)
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

static void handle_kb_delete(cJSON *params, int id, airy_sock_t client_fd)
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

static void handle_kb_list(cJSON *params, int id, airy_sock_t client_fd)
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

/* ─── 语义缓存 handlers（mem.cache_*，0.1.5） ───────────────────────────── */

static void handle_cache_put(cJSON *params, int id, airy_sock_t client_fd)
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

static void handle_cache_get(cJSON *params, int id, airy_sock_t client_fd)
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

static void handle_cache_del(cJSON *params, int id, airy_sock_t client_fd)
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

static void handle_cache_stats(int id, airy_sock_t client_fd)
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

/* ─── 上下文台账 handlers（mem.ledger_*，0.1.5） ───────────────────────── */

static int ledger_status_from_string(const char *s)
{
    if (strcmp(s, "active") == 0) return LEDGER_STATUS_ACTIVE;
    if (strcmp(s, "evicted") == 0) return LEDGER_STATUS_EVICTED;
    if (strcmp(s, "compressed") == 0) return LEDGER_STATUS_COMPRESSED;
    if (strcmp(s, "deduped") == 0) return LEDGER_STATUS_DEDUPED;
    return -1; /* 未知状态：调用方必须拒绝，防误把 ACTIVE 当目标 */
}

static int ledger_type_from_string(const char *s)
{
    if (strcmp(s, "tool_def") == 0) return LEDGER_ENTRY_TOOL_DEF;
    if (strcmp(s, "user") == 0) return LEDGER_ENTRY_USER;
    if (strcmp(s, "tool_result") == 0) return LEDGER_ENTRY_TOOL_RESULT;
    if (strcmp(s, "assistant") == 0) return LEDGER_ENTRY_ASSISTANT;
    if (strcmp(s, "compressed") == 0) return LEDGER_ENTRY_COMPRESSED;
    if (strcmp(s, "cache_hit") == 0) return LEDGER_ENTRY_CACHE_HIT;
    return LEDGER_ENTRY_SYSTEM;
}

static void handle_ledger_append(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    cJSON *entries = cJSON_GetObjectItem(params, "entries");

    if (!g_ledger || !session || !cJSON_IsString(session) || !entries || !cJSON_IsArray(entries)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "ledger_append 需 session_id + entries[]", id);
        return;
    }

    int n = cJSON_GetArraySize(entries);
    ledger_entry_in_t *in = AIRY_CALLOC(n > 0 ? (size_t)n : 1, sizeof(ledger_entry_in_t));
    if (!in) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "OOM", id);
        return;
    }
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(entries, i);
        cJSON *type = cJSON_GetObjectItem(e, "entry_type");
        cJSON *text = cJSON_GetObjectItem(e, "text");
        cJSON *token_in = cJSON_GetObjectItem(e, "token_in");
        cJSON *token_out = cJSON_GetObjectItem(e, "token_out");
        cJSON *source = cJSON_GetObjectItem(e, "source");
        cJSON *ref_id = cJSON_GetObjectItem(e, "ref_id");
        in[i].entry_type = type && cJSON_IsString(type)
                               ? ledger_type_from_string(type->valuestring)
                               : LEDGER_ENTRY_SYSTEM;
        in[i].text = text && cJSON_IsString(text) ? text->valuestring : NULL;
        in[i].token_in = token_in && cJSON_IsNumber(token_in) && token_in->valuedouble > 0
                             ? (size_t)token_in->valuedouble
                             : 0;
        in[i].token_out = token_out && cJSON_IsNumber(token_out) && token_out->valuedouble > 0
                              ? (size_t)token_out->valuedouble
                              : 0;
        in[i].source = source && cJSON_IsString(source) ? source->valuestring : NULL;
        in[i].ref_id = ref_id && cJSON_IsString(ref_id) ? ref_id->valuestring : NULL;
    }

    char *ledger_id = NULL;
    int ret = mem_ledger_append(g_ledger, session->valuestring, in, (size_t)n, &ledger_id);
    AIRY_FREE(in);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "ledger_append 失败", id);
        AIRY_FREE(ledger_id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "ledger_id", ledger_id ? ledger_id : "");
    cJSON_AddNumberToObject(result, "appended", (double)n);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    AIRY_FREE(ledger_id);
}

static void handle_ledger_window(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    if (!g_ledger || !session || !cJSON_IsString(session)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "ledger_window 需 session_id", id);
        return;
    }
    ledger_window_t win;
    int ret = mem_ledger_window(g_ledger, session->valuestring, &win);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "ledger_window 失败", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < win.count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "entry_id", win.entries[i].entry_id);
        cJSON_AddNumberToObject(item, "seq", (double)win.entries[i].seq);
        cJSON_AddNumberToObject(item, "token_in", (double)win.entries[i].token_in);
        cJSON_AddNumberToObject(item, "token_out", (double)win.entries[i].token_out);
        cJSON_AddStringToObject(item, "source", win.entries[i].source ? win.entries[i].source : "");
        cJSON_AddStringToObject(item, "ref_id", win.entries[i].ref_id ? win.entries[i].ref_id : "");
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(result, "entries", arr);
    cJSON_AddNumberToObject(result, "total_tokens", (double)win.total_tokens);
    cJSON_AddBoolToObject(result, "warn", win.warn ? 1 : 0);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    mem_ledger_window_free(&win);
}

static void handle_ledger_budget(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    if (!g_ledger || !session || !cJSON_IsString(session)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "ledger_budget 需 session_id", id);
        return;
    }
    size_t used = 0, limit = 0, headroom = 0;
    mem_ledger_budget(g_ledger, session->valuestring, &used, &limit, &headroom);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "used", (double)used);
    cJSON_AddNumberToObject(result, "limit", (double)limit);
    cJSON_AddNumberToObject(result, "headroom", (double)headroom);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_ledger_mark(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    cJSON *entry_ids = cJSON_GetObjectItem(params, "entry_ids");
    cJSON *status = cJSON_GetObjectItem(params, "status");
    if (!g_ledger || !session || !cJSON_IsString(session) || !entry_ids || !cJSON_IsArray(entry_ids) ||
        !status || !cJSON_IsString(status)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "ledger_mark 需 session_id + entry_ids[] + status", id);
        return;
    }
    int n = cJSON_GetArraySize(entry_ids);
    const char **ids = AIRY_CALLOC(n > 0 ? (size_t)n : 1, sizeof(char *));
    if (!ids) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "OOM", id);
        return;
    }
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(entry_ids, i);
        ids[i] = cJSON_IsString(e) ? e->valuestring : "";
    }
    size_t updated = 0;
    int st = ledger_status_from_string(status->valuestring);
    if (st < 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "ledger_mark 的 status 非法（active|evicted|compressed|deduped）", id);
        AIRY_FREE(ids);
        return;
    }
    int ret = mem_ledger_mark(g_ledger, session->valuestring, ids, (size_t)n, st, &updated);
    AIRY_FREE(ids);
    if (ret != AIRY_SUCCESS && ret != AIRY_ERR_NOT_FOUND) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "ledger_mark 失败", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "updated", (double)updated);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_ledger_history(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    cJSON *limit = cJSON_GetObjectItem(params, "limit");
    if (!g_ledger || !session || !cJSON_IsString(session)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "ledger_history 需 session_id", id);
        return;
    }
    ledger_entry_view_t *items = NULL;
    size_t count = 0;
    size_t lim = limit && cJSON_IsNumber(limit) ? clamp_u32(limit->valueint, 0, 10000) : 0;
    int ret = mem_ledger_history(g_ledger, session->valuestring, lim, &items, &count);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "ledger_history 失败", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON *events = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "entry_id", items[i].entry_id);
        cJSON_AddNumberToObject(item, "seq", (double)items[i].seq);
        cJSON_AddNumberToObject(item, "entry_type", (double)items[i].entry_type);
        cJSON_AddNumberToObject(item, "token_in", (double)items[i].token_in);
        cJSON_AddNumberToObject(item, "token_out", (double)items[i].token_out);
        cJSON_AddNumberToObject(item, "status", (double)items[i].status);
        cJSON_AddStringToObject(item, "source", items[i].source ? items[i].source : "");
        cJSON_AddStringToObject(item, "ref_id", items[i].ref_id ? items[i].ref_id : "");
        cJSON_AddItemToArray(events, item);
    }
    cJSON_AddItemToObject(result, "events", events);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    mem_ledger_history_free(items, count);
}

static void handle_ledger_stats(int id, airy_sock_t client_fd)
{
    if (!g_ledger) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "ledger 未初始化", id);
        return;
    }
    mem_ledger_stats_t st;
    mem_ledger_stats(g_ledger, &st);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "sessions", (double)st.sessions);
    cJSON_AddNumberToObject(result, "entries", (double)st.entries);
    cJSON_AddNumberToObject(result, "total_tokens", (double)st.total_tokens);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* 提示词压缩（mem.compress，14-prompt-compression.md §3：L1+L2 默认开）。
 * 入参：{session_id, entries:[{entry_id, entry_type, text}]}
 * 返回：{context, saved_tokens, actions:[{entry_id, entry_type, action}], marked}
 * 联动：对压缩条目 ledger.mark(COMPRESSED)，追加 compressed 块条目（可回放）。 */
static void handle_compress(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    cJSON *entries = cJSON_GetObjectItem(params, "entries");

    if (!g_ledger || !session || !cJSON_IsString(session) || !entries || !cJSON_IsArray(entries)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "compress 需 session_id + entries[]", id);
        return;
    }

    int n = cJSON_GetArraySize(entries);
    compress_entry_in_t *in = AIRY_CALLOC(n > 0 ? (size_t)n : 1, sizeof(compress_entry_in_t));
    if (!in) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "OOM", id);
        return;
    }
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(entries, i);
        cJSON *eid = cJSON_GetObjectItem(e, "entry_id");
        cJSON *etype = cJSON_GetObjectItem(e, "entry_type");
        cJSON *text = cJSON_GetObjectItem(e, "text");
        in[i].entry_id = eid && cJSON_IsString(eid) ? eid->valuestring : "";
        in[i].entry_type = etype && cJSON_IsString(etype)
                               ? ledger_type_from_string(etype->valuestring)
                               : LEDGER_ENTRY_SYSTEM;
        in[i].text = text && cJSON_IsString(text) ? text->valuestring : NULL;
        in[i].token_in = 0; /* 由 plan 用 token_standard 估算 */
    }

    char *ctx = NULL;
    size_t saved = 0;
    compress_plan_item_t *actions = NULL;
    size_t action_count = 0;
    int ret = mem_compress_plan(g_ledger, session->valuestring, in, (size_t)n, NULL, &ctx, &saved,
                                &actions, &action_count);
    AIRY_FREE(in);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "compress 失败", id);
        mem_compress_plan_free(ctx, actions, action_count);
        return;
    }

    /* 台账联动：按 action 映射状态（DROP→EVICTED / DEDUP→DEDUPED /
     * TRUNCATE|EXTRACT→COMPRESSED）。仅当有实际状态迁移（marked>0）才追加
     * 压缩块：防重复压缩已标记条目时 budget 不降反升（每次叠加新块）。 */
    size_t marked = 0;
    if (action_count > 0) {
        for (size_t i = 0; i < action_count; i++) {
            int st = LEDGER_STATUS_COMPRESSED;
            if (actions[i].action == COMPRESS_ACTION_DROP)
                st = LEDGER_STATUS_EVICTED;
            else if (actions[i].action == COMPRESS_ACTION_DEDUP)
                st = LEDGER_STATUS_DEDUPED;
            const char *one = actions[i].entry_id;
            mem_ledger_mark(g_ledger, session->valuestring, &one, 1, st, &marked);
        }
        if (marked > 0 && ctx && ctx[0]) {
            ledger_entry_in_t block = {0};
            block.entry_type = LEDGER_ENTRY_COMPRESSED;
            block.text = ctx;
            block.source = "ledger";
            block.ref_id = actions[0].entry_id;
            mem_ledger_append(g_ledger, session->valuestring, &block, 1, NULL);
        }
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "context", ctx ? ctx : "");
    cJSON_AddNumberToObject(result, "saved_tokens", (double)saved);
    cJSON_AddNumberToObject(result, "marked", (double)marked);
    cJSON *acts = cJSON_CreateArray();
    for (size_t i = 0; i < action_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "entry_id", actions[i].entry_id);
        cJSON_AddNumberToObject(item, "entry_type", (double)actions[i].entry_type);
        cJSON_AddNumberToObject(item, "action", (double)actions[i].action);
        cJSON_AddItemToArray(acts, item);
    }
    cJSON_AddItemToObject(result, "actions", acts);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    mem_compress_plan_free(ctx, actions, action_count);
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
static void handle_evolve(cJSON *params, int id, airy_sock_t client_fd)
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
            if (meta)
                cJSON_Delete(meta);
            if (sources)
                cJSON_Delete(sources);
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
                /* 拷贝 data 后立即 mem_record_free：防 rec.metadata 泄漏 */
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

static void handle_health_check(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "mem_d");
    cJSON_AddBoolToObject(result, "healthy", g_service != NULL);
    cJSON_AddNumberToObject(result, "record_count", (double)mem_service_count(g_service));
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_get_stats(int id, airy_sock_t client_fd)
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

static int load_daemon_config(const char *config_path)
{
    g_config.use_tcp = 0;
    g_config.max_clients = MAX_CLIENTS;
    g_config.max_records = MEM_DEFAULT_MAX_RECORDS;

#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_WIN);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#else
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_UNIX);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#endif
    g_config.tcp_port = DEFAULT_TCP_PORT;

    const char *env = getenv("AIRY_MEM_MAX_RECORDS");
    if (env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 65536)
            g_config.max_records = (size_t)v;
    }

    if (config_path) {
        FILE *f = fopen(config_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len > 0 && len < 1024 * 1024) {
                char *content = (char *)AIRY_MALLOC((size_t)len + 1);
                if (content) {
                    size_t read_len = fread(content, 1, (size_t)len, f);
                    if (read_len == (size_t)len) {
                        content[read_len] = '\0';
                        do {
                            CJSON_PARSE_GUARD(root, content, { break; });
                            cJSON *daemon_cfg = cJSON_GetObjectItem(root, "daemon");
                            if (daemon_cfg) {
                                cJSON *socket_path = cJSON_GetObjectItem(daemon_cfg, "socket_path");
                                if (cJSON_IsString(socket_path)) {
                                    AIRY_FREE(g_config.socket_path);
                                    g_config.socket_path = AIRY_STRDUP(socket_path->valuestring);
                                }
                                cJSON *tcp_port = cJSON_GetObjectItem(daemon_cfg, "tcp_port");
                                if (cJSON_IsNumber(tcp_port) && tcp_port->valueint > 0 &&
                                    tcp_port->valueint <= 65535) {
                                    g_config.tcp_port = (uint16_t)tcp_port->valueint;
                                    g_config.use_tcp = 1;
                                }
                                cJSON *max_clients = cJSON_GetObjectItem(daemon_cfg, "max_clients");
                                if (cJSON_IsNumber(max_clients))
                                    g_config.max_clients = max_clients->valueint;
                                cJSON *max_records = cJSON_GetObjectItem(daemon_cfg, "max_records");
                                if (cJSON_IsNumber(max_records))
                                    g_config.max_records = (size_t)max_records->valuedouble;
                            }
                        } while (0);
                    }
                    AIRY_FREE(content);
                }
            }
            fclose(f);
        }
    }
    return 0;
}

static void free_daemon_config(void)
{
    AIRY_FREE(g_config.socket_path);
    AIRY_FREE(g_config.tcp_host);
    __builtin_memset(&g_config, 0, sizeof(g_config));
}

static void destroy_service(void)
{
    if (g_service) {
        mem_service_destroy(g_service);
        g_service = NULL;
    }
    if (g_cache) {
        mem_cache_destroy(g_cache);
        g_cache = NULL;
    }
    if (g_ledger) {
        mem_ledger_destroy(g_ledger);
        g_ledger = NULL;
    }
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_mem_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_mem_d);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    DAEMON_SETUP_SIGNALS(mem_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    daemon_cupolas_init("mem_d");

    load_daemon_config(config_path);
    if (use_tcp)
        g_config.use_tcp = 1;

    SVC_LOG_INFO("Memory service starting, manager=%s", config_path ? config_path : "default");

    g_service = mem_service_create(g_config.max_records);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create memory service");
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_mem_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    /* 0.1.5：语义缓存 + 上下文台账（渐进式降级：创建失败仅告警，不阻断服务） */
    g_cache = mem_cache_create(4096, 64UL * 1024 * 1024, 3600000ULL, 0.85);
    if (!g_cache)
        SVC_LOG_WARN("Semantic cache init failed, caching disabled (degraded mode)");
    g_ledger = mem_ledger_create(0, 0);
    if (!g_ledger)
        SVC_LOG_WARN("Context ledger init failed, ledger disabled (degraded mode)");

    airy_sock_t server_fd = daemon_create_server_socket(g_config.use_tcp, g_config.tcp_port,
                                                        g_config.socket_path, g_config.socket_path);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_mem_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO(g_config.use_tcp ? "Listening on TCP %s:%d" : "Listening on %s", g_config.tcp_host,
                 g_config.tcp_port);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = 4;
    ev_config.thread_pool_max = 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_mem_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = g_config.use_tcp ? g_config.tcp_host : g_config.socket_path;
    int ret = daemon_init_event_driver("mem_d", "mem", sock_addr,
                                       g_config.use_tcp ? g_config.tcp_port : 0, "mem,core",
                                       g_config.use_tcp, &ev_config, &g_event_driver_mem_d,
                                       &g_bsd_mem_d, &g_bipc_mem_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_mem_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_mem_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_mem_d = daemon_event_driver_get_dispatcher(g_event_driver_mem_d);
    method_dispatcher_register(g_dispatcher_mem_d, "write", on_write_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "search", on_search_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "get", on_get_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "delete", on_delete_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "count", on_count_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "recent", on_recent_method, NULL);

    method_dispatcher_register(g_dispatcher_mem_d, "evolve", on_evolve_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "health_check", on_health_check_method, NULL);

    method_dispatcher_register(g_dispatcher_mem_d, "shutdown", on_shutdown_method_mem_d, NULL);

    method_dispatcher_register(g_dispatcher_mem_d, "get_stats", on_get_stats_method, NULL);

    /* 2.1.2.3：KB 知识库（RAG 一等抽象） */
    method_dispatcher_register(g_dispatcher_mem_d, "kb_ingest", on_kb_ingest_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "kb_search", on_kb_search_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "kb_delete", on_kb_delete_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "kb_list", on_kb_list_method, NULL);

    /* 0.1.5：语义缓存（13-semantic-cache-context-ledger.md §3） */
    method_dispatcher_register(g_dispatcher_mem_d, "cache_put", on_cache_put_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "cache_get", on_cache_get_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "cache_del", on_cache_del_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "cache_stats", on_cache_stats_method, NULL);

    /* 0.1.5：上下文台账（13-semantic-cache-context-ledger.md §4） */
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_append", on_ledger_append_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_window", on_ledger_window_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_budget", on_ledger_budget_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_mark", on_ledger_mark_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_history", on_ledger_history_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_stats", on_ledger_stats_method, NULL);

    /* 0.1.5：提示词压缩（14-prompt-compression.md §3 L1+L2） */
    method_dispatcher_register(g_dispatcher_mem_d, "compress", on_compress_method, NULL);
    SVC_LOG_INFO("Registered 25 RPC methods (mem.* namespace)");

    if (daemon_event_driver_add_server_fd(g_event_driver_mem_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_mem_d);
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_mem_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("Memory service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_mem_d);

    daemon_cleanup_standard(g_bipc_mem_d, g_bsd_mem_d, g_event_driver_mem_d, server_fd,
                            g_config.socket_path, destroy_service, &g_running_lock_mem_d);
    free_daemon_config();

    SVC_LOG_INFO("Memory service stopped");
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
