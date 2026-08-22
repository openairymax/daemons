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

DAEMON_DECLARE_COMMON(mem_d, mem, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(mem_d)

static mem_service_t *g_service = NULL;

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
static void handle_kb_ingest(cJSON *params, int id, airy_sock_t fd);
static void handle_kb_search(cJSON *params, int id, airy_sock_t fd);
static void handle_kb_delete(cJSON *params, int id, airy_sock_t fd);
static void handle_kb_list(cJSON *params, int id, airy_sock_t fd);

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
    uint32_t lim = limit && cJSON_IsNumber(limit) ? (uint32_t)limit->valueint : 10;

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
    uint32_t lim = limit && cJSON_IsNumber(limit) ? (uint32_t)limit->valueint : 10;

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
        uint32_t lim =
            limit_json && cJSON_IsNumber(limit_json) ? (uint32_t)limit_json->valueint : 10;

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
                data_parts[i] = (char *)rec.data;
                total_len += rec.len;
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
                                if (cJSON_IsNumber(tcp_port)) {
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

    method_dispatcher_register(g_dispatcher_mem_d, "evolve", on_evolve_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "health_check", on_health_check_method, NULL);

    method_dispatcher_register(g_dispatcher_mem_d, "shutdown", on_shutdown_method_mem_d, NULL);

    method_dispatcher_register(g_dispatcher_mem_d, "get_stats", on_get_stats_method, NULL);

    /* 2.1.2.3：KB 知识库（RAG 一等抽象） */
    method_dispatcher_register(g_dispatcher_mem_d, "kb_ingest", on_kb_ingest_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "kb_search", on_kb_search_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "kb_delete", on_kb_delete_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "kb_list", on_kb_list_method, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (mem.* namespace)", 13);

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
