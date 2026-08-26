// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file service.c
 * @brief Memory service implementation: record CRUD + vector search.
 *
 * Extracted from g_runtime.records[] logic in gateway/src/utils/syscall_router.c
 * and refactored into a standalone, self-contained service module. The
 * mem_d daemon holds a mem_service_t instance and exposes the mem.*
 * namespace over a Unix socket.
 *
 * Design notes:
 * - Own hash table (djb2, same origin as syscall_router.c but decoupled)
 * - Thread safety: all public interfaces take the lock
 * - Record ID: 32-char hex (timestamp + counter, no external deps)
 * - Search: own TF-IDF vector cosine similarity (vector.c) fused with
 *   substring scoring weighted 0.6/0.4 (weight configurable via
 *   AIRY_MEM_TFIDF_WEIGHT); optional embedding backend (emb_client.c,
 *   AIRY_MEM_EMBEDDING_URL/KEY) enhances and degrades to TF-IDF on call
 *   failure; records without a vector fall back to pure substring scoring
 *   for backward compatibility
 */

#include "service.h"

#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#endif

#define MEM_DEFAULT_MAX_RECORDS 1024
#define MEM_RECORD_ID_LEN 33
#define MEM_HASH_LOAD_FACTOR 4 /* capacity = max_records * 4 */
#define MEM_JSONL_FILENAME "mem.jsonl"
#define MEM_DEFAULT_TFIDF_WEIGHT 0.6f

static unsigned long mem_hash_fn(const char *str)
{

    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

static int mem_ht_init(mem_hash_table_t *ht, size_t capacity)
{
    if (!ht || capacity == 0)
        return AIRY_ERR_INVALID_PARAM;

    ht->entries = (mem_hash_entry_t *)AIRY_CALLOC(capacity, sizeof(mem_hash_entry_t));
    if (!ht->entries) {
        ht->capacity = 0;
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    ht->capacity = capacity;
    ht->count = 0;
    return AIRY_SUCCESS;
}

static void mem_ht_destroy(mem_hash_table_t *ht)
{
    if (!ht || !ht->entries)
        return;
    for (size_t i = 0; i < ht->capacity; i++) {
        AIRY_FREE(ht->entries[i].key);
    }
    AIRY_FREE(ht->entries);
    ht->entries = NULL;
    ht->capacity = 0;
    ht->count = 0;
}

static int mem_ht_insert(mem_hash_table_t *ht, const char *key, size_t index)
{
    if (!ht || !ht->entries || !key)
        return AIRY_ERR_INVALID_PARAM;
    if (ht->count >= ht->capacity * 3 / 4)
        return AIRY_ERR_OUT_OF_MEMORY;

    unsigned long h = mem_hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied) {
            ht->entries[pos].key = AIRY_STRDUP(key);
            if (!ht->entries[pos].key)
                return AIRY_ERR_OUT_OF_MEMORY;
            ht->entries[pos].index = index;
            ht->entries[pos].occupied = 1;
            ht->count++;
            return AIRY_SUCCESS;
        }
    }
    return AIRY_ERR_OUT_OF_MEMORY;
}

static ssize_t mem_ht_lookup(mem_hash_table_t *ht, const char *key)
{
    if (!ht || !ht->entries || !key || ht->count == 0)
        return -1;

    unsigned long h = mem_hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied)
            return -1;
        if (strcmp(ht->entries[pos].key, key) == 0)
            return (ssize_t)ht->entries[pos].index;
    }
    return -1;
}

static void mem_ht_remove(mem_hash_table_t *ht, const char *key)
{
    if (!ht || !ht->entries || !key || ht->count == 0)
        return;

    unsigned long h = mem_hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied)
            return;
        if (strcmp(ht->entries[pos].key, key) == 0) {
            AIRY_FREE(ht->entries[pos].key);
            ht->entries[pos].key = NULL;
            ht->entries[pos].occupied = 0;
            ht->entries[pos].index = 0;
            ht->count--;
            return;
        }
    }
}

static void mem_generate_record_id(char *buf, size_t buf_size)
{
    /* 32-char hex: 8-char timestamp + 8-char counter + 16-char random.
     * No external libuuid dependency, so the daemon can run standalone. */
    static uint64_t counter = 0;
    static airy_mtx_t counter_lock;
    static int counter_initialized = 0;

    if (!counter_initialized) {
        airy_mtx_init(&counter_lock);
        counter = (uint64_t)time(NULL) & 0xFFFFFFFF;
        counter_initialized = 1;
    }

    airy_mtx_lock(&counter_lock);
    uint64_t c = counter++;
    airy_mtx_unlock(&counter_lock);

    uint64_t t = (uint64_t)time(NULL);

    uint64_t r = t ^ (c * 0x9E3779B97F4A7C15ULL);
    r ^= r << 13;
    r ^= r >> 7;
    r ^= r << 17;

    if (buf_size < MEM_RECORD_ID_LEN)
        return;
    snprintf(buf, MEM_RECORD_ID_LEN, "%08lx%08lx%016lx", (unsigned long)(t & 0xFFFFFFFFu),
             (unsigned long)(c & 0xFFFFFFFFu), (unsigned long)(r & 0xFFFFFFFFFFFFFFFFULL));
}

static float mem_compute_score(const mem_record_entry_t *rec, const char *query)
{
    if (!rec || !query || !rec->data || rec->len == 0)
        return 0.0f;

    const char *text = (const char *)rec->data;
    size_t text_len = rec->len;
    size_t query_len = strlen(query);

    if (query_len == 0)
        return 0.0f;

    size_t match_count = 0;
    for (size_t i = 0; i + query_len <= text_len; i++) {
        if (strncmp(text + i, query, query_len) == 0)
            match_count++;
    }

    if (match_count == 0)
        return 0.0f;

    float density = (float)match_count * (float)query_len / (float)text_len;
    if (density > 1.0f)
        density = 1.0f;
    return density;
}

static float mem_load_tfidf_weight(void)
{
    const char *env = getenv("AIRY_MEM_TFIDF_WEIGHT");
    if (env && env[0]) {
        char *end = NULL;
        double v = strtod(env, &end);
        if (end != env && v >= 0.0 && v <= 1.0)
            return (float)v;
    }
    return MEM_DEFAULT_TFIDF_WEIGHT;
}

static void mem_record_build_vector(mem_service_t *svc, mem_record_entry_t *rec)
{
    if (!svc || !rec)
        return;
    mem_tfidf_vec_t vec;
    AIRY_MEMSET(&vec, 0, sizeof(vec));
    if (mem_vec_build((const char *)rec->data, rec->len, &vec) != AIRY_SUCCESS) {
        SVC_LOG_WARN("mem_d vector: build failed for record %s", rec->record_id);
        mem_vec_destroy(&vec);
        return;
    }
    rec->vec = vec;
    mem_df_add_doc(&svc->df_table, &rec->vec);
}

static void mem_record_free_vector(mem_record_entry_t *rec)
{
    if (!rec)
        return;
    mem_vec_destroy(&rec->vec);
    AIRY_FREE(rec->emb);
    rec->emb = NULL;
    rec->emb_dim = 0;
}

/* Escape JSON string literal content (excluding the outer quotes).
 * Returns a malloc'd buffer (caller AIRY_FREE) and the written length.
 * On failure *out = NULL and returns 0. */
static size_t mem_json_escape(const char *in, size_t len, char **out)
{
    if (!out)
        return 0;
    *out = NULL;
    if (!in || len == 0) {
        *out = (char *)AIRY_MALLOC(1);
        if (*out)
            (*out)[0] = '\0';
        return 0;
    }
    size_t cap = len * 6 + 1;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf)
        return 0;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':
            buf[pos++] = '\\';
            buf[pos++] = '"';
            break;
        case '\\':
            buf[pos++] = '\\';
            buf[pos++] = '\\';
            break;
        case '\n':
            buf[pos++] = '\\';
            buf[pos++] = 'n';
            break;
        case '\t':
            buf[pos++] = '\\';
            buf[pos++] = 't';
            break;
        case '\r':
            buf[pos++] = '\\';
            buf[pos++] = 'r';
            break;
        case '\b':
            buf[pos++] = '\\';
            buf[pos++] = 'b';
            break;
        case '\f':
            buf[pos++] = '\\';
            buf[pos++] = 'f';
            break;
        default:
            if (c < 0x20) {
                int w = snprintf(buf + pos, cap - pos, "\\u%04x", c);
                if (w > 0)
                    pos += (size_t)w;
            } else {
                buf[pos++] = (char)c;
            }
            break;
        }
    }
    buf[pos] = '\0';
    *out = buf;
    return pos;
}

static int mem_jsonl_path_resolve(char **out_path)
{
    if (!out_path)
        return AIRY_ERR_INVALID_PARAM;
    *out_path = NULL;

    /* 持久记忆数据落 data/（run/ 仅承载 socket/pid 等易失运行时文件；
     * 记忆随会话留存，属持久数据，与 heapstore/hall 同一分区）。 */
    const char *dir = airy_data_dir();
    if (!dir || !dir[0])
        return AIRY_ERR_INVALID_PARAM;

    /* 确保完整父目录存在：仅 airy_mkdir_p(dir) 只建了 data 目录，
     * agentrt/memory 子目录缺失时首次运行 append/rewrite 静默失败
     * （errno=2），记忆不落盘。必须递归创建完整父路径。 */
    size_t dir_len = strlen(dir);
    size_t need = dir_len + 1 + strlen("agentrt") + 1 + strlen("memory") + 1 +
                  strlen(MEM_JSONL_FILENAME) + 1;
    char *path = (char *)AIRY_MALLOC(need);
    if (!path)
        return AIRY_ERR_OUT_OF_MEMORY;
    snprintf(path, need, "%s/agentrt/memory/%s", dir, MEM_JSONL_FILENAME);

    char parent[4096];
    snprintf(parent, sizeof(parent), "%s/agentrt/memory", dir);
    (void)airy_mkdir_p(parent);

    *out_path = path;
    return AIRY_SUCCESS;
}

static void mem_persist_append_record(mem_service_t *svc, const mem_record_entry_t *rec)
{
    if (!svc || !svc->jsonl_path || !rec || !rec->record_id || !rec->data)
        return;

    if (!svc->jsonl_append_fp) {
        svc->jsonl_append_fp = fopen(svc->jsonl_path, "a");
        if (!svc->jsonl_append_fp) {
            SVC_LOG_WARN("mem_d persist: open append failed (path=%s errno=%d)", svc->jsonl_path,
                         errno);
            return;
        }
    }

    char *data_esc = NULL;
    mem_json_escape((const char *)rec->data, rec->len, &data_esc);
    if (!data_esc) {
        SVC_LOG_WARN("mem_d persist: json escape failed (len=%zu)", rec->len);
        return;
    }

    const char *meta = rec->metadata ? rec->metadata : "null";
    int rc = fprintf(
        svc->jsonl_append_fp,
        "{\"record_id\":\"%s\",\"data\":\"%s\",\"metadata\":%s,\"created_at\":%llu,\"len\":%zu}\n",
        rec->record_id, data_esc, meta, (unsigned long long)rec->created_at, rec->len);
    if (rc < 0) {
        SVC_LOG_WARN("mem_d persist: append write failed (errno=%d)", errno);
    } else {
        if (fflush(svc->jsonl_append_fp) != 0) {
            SVC_LOG_WARN("mem_d persist: fflush failed (errno=%d)", errno);
        }
    }
    AIRY_FREE(data_esc);
}

static void mem_persist_rewrite_all(mem_service_t *svc)
{
    if (!svc || !svc->jsonl_path)
        return;

    if (svc->jsonl_append_fp) {
        fclose(svc->jsonl_append_fp);
        svc->jsonl_append_fp = NULL;
    }

    /* P2: write to a temp file in the same directory, fsync, then rename over
     * the target so a crash mid-rewrite never truncates the whole memory
     * store. */
    char tmppath[4096];
    if (snprintf(tmppath, sizeof(tmppath), "%s.tmp", svc->jsonl_path) >=
        (int)sizeof(tmppath)) {
        SVC_LOG_WARN("mem_d persist: rewrite tmp path too long (path=%s)", svc->jsonl_path);
        return;
    }

    FILE *f = fopen(tmppath, "w");
    if (!f) {
        SVC_LOG_WARN("mem_d persist: open rewrite tmp failed (path=%s errno=%d)", tmppath,
                     errno);
        return;
    }

    for (size_t i = 0; i < svc->record_count; i++) {
        const mem_record_entry_t *rec = &svc->records[i];
        if (!rec->record_id || !rec->data)
            continue;
        char *data_esc = NULL;
        mem_json_escape((const char *)rec->data, rec->len, &data_esc);
        if (!data_esc)
            continue;
        const char *meta = rec->metadata ? rec->metadata : "null";
        int wrc = fprintf(f,
                          "{\"record_id\":\"%s\",\"data\":\"%s\",\"metadata\":%s,\"created_at\":"
                          "%llu,\"len\":%zu}\n",
                          rec->record_id, data_esc, meta, (unsigned long long)rec->created_at,
                          rec->len);
        AIRY_FREE(data_esc);
        if (wrc < 0) {
            /* 写失败（ENOSPC/EIO）：终止全量重写并告警，避免静默丢记忆 */
            SVC_LOG_WARN("mem_d persist: rewrite write failed (errno=%d)", errno);
            fclose(f);
            remove(tmppath);
            return;
        }
    }

    int failed = (fflush(f) != 0);
#ifndef _WIN32
    if (!failed) {
        int fd = fileno(f);
        if (fd >= 0 && fsync(fd) != 0)
            failed = 1;
    }
#else
    if (!failed) {
        int fd = _fileno(f);
        if (fd >= 0 && _commit(fd) != 0)
            failed = 1;
    }
#endif
    if (fclose(f) != 0)
        failed = 1;

    if (failed) {
        SVC_LOG_WARN("mem_d persist: rewrite flush failed (errno=%d)", errno);
        remove(tmppath);
        return;
    }

#ifndef _WIN32
    if (rename(tmppath, svc->jsonl_path) != 0) {
#else
    if (!MoveFileExA(tmppath, svc->jsonl_path, MOVEFILE_REPLACE_EXISTING)) {
#endif
        SVC_LOG_WARN("mem_d persist: rewrite rename failed (errno=%d)", errno);
        remove(tmppath);
        return;
    }
}

static void mem_persist_load_existing(mem_service_t *svc)
{
    if (!svc || !svc->jsonl_path)
        return;

    FILE *f = fopen(svc->jsonl_path, "r");
    if (!f)
        return;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long fsize = ftell(f);
    if (fsize <= 0) {
        fclose(f);
        return;
    }
    rewind(f);

    char *content = (char *)AIRY_MALLOC((size_t)fsize + 1);
    if (!content) {
        SVC_LOG_WARN("mem_d persist: load alloc failed (%ld bytes)", fsize);
        fclose(f);
        return;
    }
    size_t read_len = fread(content, 1, (size_t)fsize, f);
    fclose(f);
    content[read_len] = '\0';

    size_t loaded = 0;
    char *line_start = content;
    for (size_t i = 0; i <= read_len; i++) {
        if (i == read_len || content[i] == '\n') {
            content[i] = '\0';
            size_t llen = strlen(line_start);
            while (llen > 0 && line_start[llen - 1] == '\r') {
                line_start[--llen] = '\0';
            }
            if (llen > 0) {
                cJSON *root = cJSON_Parse(line_start);
                if (!root) {
                    SVC_LOG_WARN("mem_d persist: skip malformed jsonl line");
                } else {
                    cJSON *rid = cJSON_GetObjectItem(root, "record_id");
                    cJSON *data = cJSON_GetObjectItem(root, "data");
                    cJSON *meta = cJSON_GetObjectItem(root, "metadata");
                    cJSON *cat = cJSON_GetObjectItem(root, "created_at");
                    cJSON *len_item = cJSON_GetObjectItem(root, "len");

                    if (cJSON_IsString(rid) && cJSON_IsString(data) &&
                        svc->record_count < svc->max_records) {
                        size_t idx = svc->record_count;
                        mem_record_entry_t *rec = &svc->records[idx];
                        const char *data_str = data->valuestring;
                        size_t data_len = (len_item && cJSON_IsNumber(len_item)) ?
                                              (size_t)len_item->valuedouble :
                                              strlen(data_str);
                        if (data_len == 0)
                            data_len = strlen(data_str);

                        rec->record_id = AIRY_STRDUP(rid->valuestring);
                        rec->data = AIRY_MALLOC(data_len + 1);
                        if (rec->record_id && rec->data) {
                            __builtin_memcpy(rec->data, data_str, data_len);
                            ((char *)rec->data)[data_len] = '\0';
                            rec->len = data_len;
                            /* metadata can be any JSON value (object/array/
                             * string etc.); reserialize as-is with
                             * cJSON_PrintUnformatted to guarantee round-trip */
                            if (meta && !cJSON_IsNull(meta)) {
                                char *meta_str = cJSON_PrintUnformatted(meta);
                                if (meta_str) {
                                    rec->metadata = AIRY_STRDUP(meta_str);
                                    cJSON_free(meta_str);
                                }
                            }
                            rec->score = 0.0f;
                            rec->created_at = (cat && cJSON_IsNumber(cat)) ?
                                                  (uint64_t)cat->valuedouble :
                                                  (uint64_t)time(NULL);
                            if (mem_ht_insert(&svc->record_index, rec->record_id, idx) ==
                                AIRY_SUCCESS) {
                                /* On restart, rebuild the TF-IDF vector from
                                 * the JSONL original text and register the
                                 * global DF (JSONL persists only the original
                                 * text; vectors live in memory only) */
                                mem_record_build_vector(svc, rec);
                                svc->record_count++;
                                loaded++;
                            } else {
                                AIRY_FREE(rec->record_id);
                                AIRY_FREE(rec->data);
                                AIRY_FREE(rec->metadata);
                                rec->record_id = NULL;
                                rec->data = NULL;
                                rec->metadata = NULL;
                            }
                        } else {
                            AIRY_FREE(rec->record_id);
                            AIRY_FREE(rec->data);
                            rec->record_id = NULL;
                            rec->data = NULL;
                        }
                    }
                    cJSON_Delete(root);
                }
            }
            line_start = content + i + 1;
        }
    }

    AIRY_FREE(content);
    if (loaded > 0)
        SVC_LOG_INFO("mem_d persist: loaded %zu records from %s", loaded, svc->jsonl_path);
}

mem_service_t *mem_service_create(size_t max_records)
{
    if (max_records == 0)
        max_records = MEM_DEFAULT_MAX_RECORDS;

    mem_service_t *svc = (mem_service_t *)AIRY_CALLOC(1, sizeof(mem_service_t));
    if (!svc)
        return NULL;

    svc->max_records = max_records;
    svc->records = (mem_record_entry_t *)AIRY_CALLOC(max_records, sizeof(mem_record_entry_t));
    if (!svc->records) {
        AIRY_FREE(svc);
        return NULL;
    }

    if (mem_ht_init(&svc->record_index, max_records * MEM_HASH_LOAD_FACTOR) != AIRY_SUCCESS) {
        AIRY_FREE(svc->records);
        AIRY_FREE(svc);
        return NULL;
    }

    if (mem_df_init(&svc->df_table, max_records * 64) != AIRY_SUCCESS) {
        mem_ht_destroy(&svc->record_index);
        AIRY_FREE(svc->records);
        AIRY_FREE(svc);
        return NULL;
    }

    airy_mtx_init(&svc->lock);
    svc->record_count = 0;
    svc->initialized = 1;
    svc->tfidf_weight = mem_load_tfidf_weight();
    (void)mem_emb_client_init(&svc->emb);

    if (mem_jsonl_path_resolve(&svc->jsonl_path) != AIRY_SUCCESS) {
        SVC_LOG_WARN("mem_d persist: path resolve failed, persistence disabled");
        svc->jsonl_path = NULL;
    } else {
        mem_persist_load_existing(svc);
    }

    SVC_LOG_INFO("Memory service created (max_records=%zu, tfidf_weight=%.2f)", max_records,
                 (double)svc->tfidf_weight);
    return svc;
}

void mem_service_destroy(mem_service_t *svc)
{
    if (!svc)
        return;

    airy_mtx_lock(&svc->lock);
    for (size_t i = 0; i < svc->record_count; i++) {
        mem_record_free_vector(&svc->records[i]);
        AIRY_FREE(svc->records[i].record_id);
        AIRY_FREE(svc->records[i].data);
        AIRY_FREE(svc->records[i].metadata);
    }
    AIRY_FREE(svc->records);
    mem_ht_destroy(&svc->record_index);
    mem_df_destroy(&svc->df_table);
    mem_emb_client_destroy(&svc->emb);

    if (svc->jsonl_append_fp) {
        fflush(svc->jsonl_append_fp);
        fclose(svc->jsonl_append_fp);
        svc->jsonl_append_fp = NULL;
    }
    AIRY_FREE(svc->jsonl_path);
    svc->jsonl_path = NULL;
    svc->record_count = 0;
    svc->max_records = 0;
    svc->initialized = 0;
    airy_mtx_unlock(&svc->lock);
    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

int mem_service_write(mem_service_t *svc, const mem_write_request_t *req, char **out_record_id)
{
    if (!svc || !svc->initialized || !req || !req->data || req->len == 0 || !out_record_id)
        return AIRY_ERR_INVALID_PARAM;

    *out_record_id = NULL;

    airy_mtx_lock(&svc->lock);

    if (svc->record_count >= svc->max_records) {
        airy_mtx_unlock(&svc->lock);
        SVC_LOG_WARN("Memory service full (count=%zu)", svc->record_count);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t idx = svc->record_count;
    mem_record_entry_t *rec = &svc->records[idx];

    char id_buf[MEM_RECORD_ID_LEN];
    mem_generate_record_id(id_buf, sizeof(id_buf));
    rec->record_id = AIRY_STRDUP(id_buf);
    rec->data = AIRY_MALLOC(req->len + 1);
    if (!rec->record_id || !rec->data) {
        AIRY_FREE(rec->record_id);
        AIRY_FREE(rec->data);
        rec->record_id = NULL;
        rec->data = NULL;
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    __builtin_memcpy(rec->data, req->data, req->len);
    ((char *)rec->data)[req->len] = '\0';
    rec->len = req->len;
    rec->metadata = req->metadata ? AIRY_STRDUP(req->metadata) : NULL;
    rec->score = 0.0f;
    rec->created_at = (uint64_t)time(NULL);

    mem_record_build_vector(svc, rec);

    int rc = mem_ht_insert(&svc->record_index, rec->record_id, idx);
    if (rc != AIRY_SUCCESS) {
        mem_record_free_vector(rec);
        AIRY_FREE(rec->record_id);
        AIRY_FREE(rec->data);
        AIRY_FREE(rec->metadata);
        rec->record_id = NULL;
        rec->data = NULL;
        rec->metadata = NULL;
        airy_mtx_unlock(&svc->lock);
        return rc;
    }

    *out_record_id = AIRY_STRDUP(rec->record_id);
    svc->record_count++;
    uint64_t total_writes = svc->record_count;
    (void)total_writes; /* 仅用于 DEBUG 日志（Release 下宏为空，避免 unused） */

    mem_persist_append_record(svc, rec);

    /* 解锁前先拷贝内容到本地缓冲：rec->data 属于服务内部记录，并发
     * mem_service_delete 在锁内 AIRY_FREE(rec->data) 并搬移数组，锁外
     * 再读 rec->data 构成 use-after-free（P0，2026-08-25 修复）。仅在
     * 确实需要 embedding 时拷贝，避免无 embedding 后端时的浪费。 */
    char *data_copy = NULL;
    if (mem_emb_should_try(&svc->emb) && rec->data && rec->len > 0) {
        data_copy = (char *)AIRY_MALLOC(rec->len + 1);
        if (data_copy) {
            __builtin_memcpy(data_copy, rec->data, rec->len);
            data_copy[rec->len] = '\0';
        }
    }

    airy_mtx_unlock(&svc->lock);

    /* Optional embedding enhancement: make the network call outside the lock
     * (avoid blocking while holding it); on success backfill the vector; on
     * failure emb_client already marks unhealthy and cools down, degrading to
     * TF-IDF automatically */
    if (data_copy) {
        float *emb_vec = NULL;
        size_t emb_dim = 0;
        if (mem_emb_embed(&svc->emb, data_copy, &emb_vec, &emb_dim) == AIRY_SUCCESS &&
            emb_vec && emb_dim > 0) {
            airy_mtx_lock(&svc->lock);
            ssize_t eidx = mem_ht_lookup(&svc->record_index, *out_record_id);
            if (eidx >= 0 && (size_t)eidx < svc->record_count) {
                AIRY_FREE(svc->records[eidx].emb);
                svc->records[eidx].emb = emb_vec;
                svc->records[eidx].emb_dim = emb_dim;
                emb_vec = NULL;
            }
            airy_mtx_unlock(&svc->lock);
        }
        AIRY_FREE(data_copy);
        AIRY_FREE(emb_vec);
    }

    SVC_LOG_DEBUG("Memory write: record_id=%s, len=%zu, total=%lu", *out_record_id, req->len,
                  (unsigned long)total_writes);
    return AIRY_SUCCESS;
}

/* ==================== KB (knowledge base) ====================
 *
 * 2.1.2.3：RAG 知识库一等抽象。开发者通过 kb.* 命名空间把文档摄入
 * （自动 UTF-8 安全分块）、在库内检索、整库删除与列出，作为 mem_d
 * 之上的"知识库"层，复用既有 record 存储/TF-IDF+embedding 混合检索。
 * 每条 chunk 记录的 metadata 携带 {"kb_id":..,"doc_id":..,"chunk":N}。
 */

#define MEM_KB_DEFAULT_CHUNK_SIZE 512

/* 共享检索核心：kb_id 非空时仅在属于该知识库的记录中打分。 */
static int mem_service_search_filtered(mem_service_t *svc, const char *kb_id, const char *query,
                                       uint32_t limit, mem_search_hit_t **out_hits,
                                       size_t *out_count);

/* 交换删除指定下标记录（见定义处注释）；mem_service_kb_delete 复用 */
static void mem_remove_record_at(mem_service_t *svc, size_t idx);

/* 解析 record 的 metadata JSON，返回其中 kb_id 字符串（静态缓冲）。
 * 非 KB 记录（无 kb_id 字段）返回 NULL。 */
static const char *mem_rec_kb_id(const mem_record_entry_t *rec)
{
    if (!rec || !rec->metadata || !rec->metadata[0])
        return NULL;
    cJSON *root = cJSON_Parse(rec->metadata);
    if (!root)
        return NULL;
    cJSON *kb = cJSON_GetObjectItem(root, "kb_id");
    const char *id = (cJSON_IsString(kb) && kb->valuestring) ? kb->valuestring : NULL;
    if (id) {
        static char s_kb_buf[256];
        size_t n = strlen(id);
        if (n >= sizeof(s_kb_buf))
            n = sizeof(s_kb_buf) - 1;
        __builtin_memcpy(s_kb_buf, id, n);
        s_kb_buf[n] = '\0';
        cJSON_Delete(root);
        return s_kb_buf;
    }
    cJSON_Delete(root);
    return NULL;
}

static int mem_rec_in_kb(const mem_record_entry_t *rec, const char *kb_id)
{
    const char *id = mem_rec_kb_id(rec);
    return id && strcmp(id, kb_id) == 0;
}

/* UTF-8 安全分块：从 off 起取至多 chunk_size 字节，若切点落在多字节
 * 序列的连续字节（0x80-0xBF）中间则回退到该序列首字节，保证 chunk
 * 不以半个字符结尾。返回本块字节数（>0）；文本取尽返回 0。 */
static size_t mem_kb_chunk_len(const unsigned char *text, size_t len, size_t off,
                               size_t chunk_size)
{
    size_t end = off + chunk_size;
    if (end >= len) {
        return len - off; /* 末块（可能为空，调用方据此终止） */
    }
    size_t p = end;
    while (p > off && (text[p] & 0xC0) == 0x80) /* 连续字节 → 回退到首字节 */
        p--;
    return p - off;
}

static int mem_kb_build_meta(const char *kb_id, const char *doc_id, size_t chunk, char **out_meta)
{
    cJSON *meta = cJSON_CreateObject();
    if (!meta)
        return AIRY_ERR_OUT_OF_MEMORY;
    cJSON_AddStringToObject(meta, "kb_id", kb_id);
    cJSON_AddStringToObject(meta, "doc_id", doc_id ? doc_id : "");
    cJSON_AddNumberToObject(meta, "chunk", (double)chunk);
    char *s = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!s)
        return AIRY_ERR_OUT_OF_MEMORY;
    *out_meta = s;
    return AIRY_SUCCESS;
}

int mem_service_kb_ingest(mem_service_t *svc, const char *kb_id, const char *doc_id,
                          const void *text, size_t len, size_t chunk_size, size_t *out_count)
{
    if (!svc || !svc->initialized || !kb_id || !kb_id[0] || !text || len == 0)
        return AIRY_ERR_INVALID_PARAM;
    if (chunk_size == 0)
        chunk_size = MEM_KB_DEFAULT_CHUNK_SIZE;

    const unsigned char *bytes = (const unsigned char *)text;
    size_t off = 0;
    size_t written = 0;
    size_t chunk_idx = 0;
    int rc = AIRY_SUCCESS;

    while (off < len) {
        size_t clen = mem_kb_chunk_len(bytes, len, off, chunk_size);
        if (clen == 0)
            break;

        char *meta = NULL;
        rc = mem_kb_build_meta(kb_id, doc_id, chunk_idx, &meta);
        if (rc != AIRY_SUCCESS)
            break;

        mem_write_request_t req = {.data = bytes + off, .len = clen, .metadata = meta};
        char *rid = NULL;
        int wret = mem_service_write(svc, &req, &rid);
        AIRY_FREE(meta);
        AIRY_FREE(rid);

        if (wret != AIRY_SUCCESS) {
            rc = wret;
            break;
        }
        written++;
        chunk_idx++;
        off += clen;
    }

    if (out_count)
        *out_count = written;
    return rc;
}

int mem_service_kb_search(mem_service_t *svc, const char *kb_id, const char *query,
                          uint32_t limit, mem_search_hit_t **out_hits, size_t *out_count)
{
    return mem_service_search_filtered(svc, kb_id, query, limit, out_hits, out_count);
}

int mem_service_kb_delete(mem_service_t *svc, const char *kb_id, size_t *out_deleted)
{
    if (!svc || !svc->initialized || !kb_id || !kb_id[0])
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);

    /* 逐条交换删除：mem_remove_record_at 将尾部记录搬入当前位置，
     * 因此 i 不可自增（须重查当前位置），保证数组始终紧凑、
     * 无游离记录（record_count 语义正确，destroy 可完整回收）。 */
    size_t deleted = 0;
    for (size_t i = 0; i < svc->record_count;) {
        if (mem_rec_in_kb(&svc->records[i], kb_id)) {
            mem_remove_record_at(svc, i);
            deleted++;
        } else {
            i++;
        }
    }
    if (deleted > 0)
        mem_persist_rewrite_all(svc);

    airy_mtx_unlock(&svc->lock);
    if (out_deleted)
        *out_deleted = deleted;
    SVC_LOG_INFO("mem.kb_delete: kb=%s deleted=%zu", kb_id, deleted);
    return AIRY_SUCCESS;
}

int mem_service_kb_list(mem_service_t *svc, char ***out_kb_ids, size_t *out_count)
{
    if (!svc || !svc->initialized || !out_kb_ids || !out_count)
        return AIRY_ERR_INVALID_PARAM;
    *out_kb_ids = NULL;
    *out_count = 0;

    airy_mtx_lock(&svc->lock);
    char **ids = NULL;
    size_t count = 0;
    for (size_t i = 0; i < svc->record_count; i++) {
        const char *kb = mem_rec_kb_id(&svc->records[i]);
        if (!kb)
            continue;
        int dup = 0;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(ids[j], kb) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            char **grown = (char **)AIRY_REALLOC(ids, (count + 1) * sizeof(char *));
            if (!grown) {
                for (size_t j = 0; j < count; j++)
                    AIRY_FREE(ids[j]);
                AIRY_FREE(ids);
                airy_mtx_unlock(&svc->lock);
                return AIRY_ERR_OUT_OF_MEMORY;
            }
            ids = grown;
            ids[count] = AIRY_STRDUP(kb);
            if (!ids[count]) {
                for (size_t j = 0; j < count; j++)
                    AIRY_FREE(ids[j]);
                AIRY_FREE(ids);
                airy_mtx_unlock(&svc->lock);
                return AIRY_ERR_OUT_OF_MEMORY;
            }
            count++;
        }
    }
    airy_mtx_unlock(&svc->lock);

    *out_kb_ids = ids;
    *out_count = count;
    return AIRY_SUCCESS;
}

void mem_kb_list_free(char **kb_ids, size_t count)
{
    if (!kb_ids)
        return;
    for (size_t i = 0; i < count; i++)
        AIRY_FREE(kb_ids[i]);
    AIRY_FREE(kb_ids);
}

int mem_service_search(mem_service_t *svc, const char *query, uint32_t limit,
                       mem_search_hit_t **out_hits, size_t *out_count)
{
    return mem_service_search_filtered(svc, NULL, query, limit, out_hits, out_count);
}

/* 共享检索核心：kb_id 非空时仅在属于该知识库的记录中打分。 */
static int mem_service_search_filtered(mem_service_t *svc, const char *kb_id, const char *query,
                                       uint32_t limit, mem_search_hit_t **out_hits,
                                       size_t *out_count)
{
    if (!svc || !svc->initialized || !query || !out_hits || !out_count)
        return AIRY_ERR_INVALID_PARAM;

    *out_hits = NULL;
    *out_count = 0;

    if (limit == 0)
        limit = 10;

    /* Prepare outside the lock (network calls do not hold the lock, avoiding
     * blocking writes/deletes):
     * 1) query the TF-IDF term vector; 2) optionally query embedding
     * (degrades automatically on failure) */
    mem_tfidf_vec_t qvec;
    AIRY_MEMSET(&qvec, 0, sizeof(qvec));
    (void)mem_vec_build(query, strlen(query), &qvec);

    float *qemb = NULL;
    size_t qemb_dim = 0;
    int use_emb = 0;
    if (mem_emb_should_try(&svc->emb) &&
        mem_emb_embed(&svc->emb, query, &qemb, &qemb_dim) == AIRY_SUCCESS && qemb && qemb_dim > 0)
        use_emb = 1;

    airy_mtx_lock(&svc->lock);

    if (svc->record_count == 0) {
        airy_mtx_unlock(&svc->lock);
        mem_vec_destroy(&qvec);
        AIRY_FREE(qemb);
        return AIRY_SUCCESS;
    }

    mem_search_hit_t *hits =
        (mem_search_hit_t *)AIRY_CALLOC(svc->record_count, sizeof(mem_search_hit_t));
    if (!hits) {
        airy_mtx_unlock(&svc->lock);
        mem_vec_destroy(&qvec);
        AIRY_FREE(qemb);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t hit_count = 0;
    size_t vec_used = 0;
    const float w = svc->tfidf_weight;
    for (size_t i = 0; i < svc->record_count; i++) {
        const mem_record_entry_t *rec = &svc->records[i];

        /* KB 过滤：仅对属于该知识库的记录打分 */
        if (kb_id && !mem_rec_in_kb(rec, kb_id))
            continue;

        /* Vector similarity: use embedding cosine when embedding is available
         * and the record has a vector, otherwise TF-IDF cosine; when the
         * record has no vector (or the query has no terms) this component is 0 */
        float vec_score = 0.0f;
        if (use_emb && rec->emb && rec->emb_dim == qemb_dim) {
            vec_score = mem_emb_cosine(qemb, rec->emb, qemb_dim);
        } else if (rec->vec.term_count > 0) {
            vec_score = mem_vec_cosine(&qvec, &rec->vec, &svc->df_table, svc->record_count);
        }
        if (rec->vec.term_count > 0)
            vec_used++;

        float sub_score = mem_compute_score(rec, query);

        float score = w * vec_score + (1.0f - w) * sub_score;
        if (score > 0.0f) {
            hits[hit_count].record_id = AIRY_STRDUP(rec->record_id);
            hits[hit_count].score = score;
            hit_count++;
        }
    }

    /* Sort all hits by score descending first (bubble sort; small scale,
     * O(N^2) acceptable), then truncate to the first limit entries, avoiding
     * high-scoring hits being wrongly dropped */
    for (size_t i = 0; i + 1 < hit_count; i++) {
        for (size_t j = 0; j + 1 < hit_count - i; j++) {
            if (hits[j].score < hits[j + 1].score) {
                mem_search_hit_t tmp = hits[j];
                hits[j] = hits[j + 1];
                hits[j + 1] = tmp;
            }
        }
    }

    if (hit_count > limit) {
        for (size_t i = limit; i < hit_count; i++) {
            AIRY_FREE(hits[i].record_id);
            hits[i].record_id = NULL;
            hits[i].score = 0.0f;
        }
        hit_count = limit;
    }

    airy_mtx_unlock(&svc->lock);

    mem_vec_destroy(&qvec);
    AIRY_FREE(qemb);

    if (hit_count == 0) {
        AIRY_FREE(hits);
        *out_hits = NULL;
        *out_count = 0;
        return AIRY_SUCCESS;
    }

    /* Shrink to the actual hit count (do the multiplication overflow check
     * first to avoid hit_count * sizeof wrapping; on overflow keep the
     * original buffer, no shrink) */
    mem_search_hit_t *shrunk = NULL;
    if (hit_count <= SIZE_MAX / sizeof(mem_search_hit_t)) {
        shrunk = (mem_search_hit_t *)AIRY_REALLOC(hits, hit_count * sizeof(mem_search_hit_t));
    }
    *out_hits = shrunk ? shrunk : hits;
    *out_count = hit_count;

    SVC_LOG_DEBUG("Memory search: query='%s', hits=%zu (vec_used=%zu)", query, hit_count, vec_used);
    return AIRY_SUCCESS;
}

int mem_service_get(mem_service_t *svc, const char *record_id, mem_record_t *out_record)
{
    if (!svc || !svc->initialized || !record_id || !out_record)
        return AIRY_ERR_INVALID_PARAM;

    __builtin_memset(out_record, 0, sizeof(*out_record));

    airy_mtx_lock(&svc->lock);

    ssize_t idx = mem_ht_lookup(&svc->record_index, record_id);
    if (idx < 0 || (size_t)idx >= svc->record_count) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }

    mem_record_entry_t *rec = &svc->records[idx];
    out_record->data = AIRY_MALLOC(rec->len + 1);
    if (!out_record->data) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    __builtin_memcpy(out_record->data, rec->data, rec->len);
    ((char *)out_record->data)[rec->len] = '\0';
    out_record->len = rec->len;
    out_record->metadata = rec->metadata ? AIRY_STRDUP(rec->metadata) : NULL;

    airy_mtx_unlock(&svc->lock);
    return AIRY_SUCCESS;
}

/**
 * @brief 交换删除指定下标记录（调用方须持有 svc->lock）。
 *
 * 释放向量/字段，将尾部记录搬入当前位置（数组保持紧凑），并同步
 * 维护 DF 表与 record_index 哈希。既有 mem_service_delete 与
 * mem_service_kb_delete 共用此路径，避免两套删除语义不一致。
 */
static void mem_remove_record_at(mem_service_t *svc, size_t idx)
{
    mem_record_entry_t *rec = &svc->records[idx];
    const char *removed_id = rec->record_id;

    /* 先移除哈希索引（removed_id 此时仍有效），再释放各字段 */
    mem_ht_remove(&svc->record_index, removed_id);

    mem_df_remove_doc(&svc->df_table, &rec->vec);
    mem_record_free_vector(rec);

    AIRY_FREE(rec->record_id);
    AIRY_FREE(rec->data);
    AIRY_FREE(rec->metadata);

    size_t last = svc->record_count - 1;
    if (idx != last) {
        svc->records[idx] = svc->records[last];
        /* 尾部记录被搬入 idx，更新其哈希索引指向 */
        mem_ht_remove(&svc->record_index, svc->records[idx].record_id);
        mem_ht_insert(&svc->record_index, svc->records[idx].record_id, idx);
    }
    __builtin_memset(&svc->records[last], 0, sizeof(mem_record_entry_t));
    svc->record_count--;
}

int mem_service_delete(mem_service_t *svc, const char *record_id)
{
    if (!svc || !svc->initialized || !record_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);

    ssize_t idx = mem_ht_lookup(&svc->record_index, record_id);
    if (idx < 0 || (size_t)idx >= svc->record_count) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }

    mem_remove_record_at(svc, (size_t)idx);
    mem_persist_rewrite_all(svc);

    airy_mtx_unlock(&svc->lock);

    SVC_LOG_DEBUG("Memory delete: record_id=%s, remaining=%zu", record_id, svc->record_count);
    return AIRY_SUCCESS;
}

size_t mem_service_count(mem_service_t *svc)
{
    if (!svc || !svc->initialized)
        return 0;
    airy_mtx_lock(&svc->lock);
    size_t c = svc->record_count;
    airy_mtx_unlock(&svc->lock);
    return c;
}

#define MEM_RECENT_DEFAULT_LIMIT 10

int mem_service_recent(mem_service_t *svc, uint32_t limit,
                       mem_recent_item_t **out_items, size_t *out_count)
{
    if (!svc || !svc->initialized || !out_items || !out_count)
        return AIRY_ERR_INVALID_PARAM;
    *out_items = NULL;
    *out_count = 0;

    if (limit == 0)
        limit = MEM_RECENT_DEFAULT_LIMIT;

    airy_mtx_lock(&svc->lock);

    size_t total = svc->record_count;
    if (total == 0) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_SUCCESS;
    }

    size_t take = total < limit ? total : limit;
    /* 越界防溢出（take 已 ≤ total，total 受 max_records 约束，安全） */
    mem_recent_item_t *items =
        (mem_recent_item_t *)AIRY_CALLOC(take, sizeof(mem_recent_item_t));
    if (!items) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t n = 0;
    /* 数组按写入序排列，尾部最新；倒序返回（新 → 旧） */
    for (size_t i = 0; i < take; i++) {
        const mem_record_entry_t *rec = &svc->records[total - 1 - i];
        if (!rec->record_id || !rec->data)
            continue;
        mem_recent_item_t *it = &items[n];
        it->record_id = AIRY_STRDUP(rec->record_id);
        it->data = AIRY_MALLOC(rec->len + 1);
        if (!it->record_id || !it->data) {
            AIRY_FREE(it->record_id);
            AIRY_FREE(it->data);
            it->record_id = NULL;
            it->data = NULL;
            continue;
        }
        __builtin_memcpy(it->data, rec->data, rec->len);
        ((char *)it->data)[rec->len] = '\0';
        it->len = rec->len;
        it->metadata = rec->metadata ? AIRY_STRDUP(rec->metadata) : NULL;
        it->created_at = rec->created_at;
        n++;
    }

    airy_mtx_unlock(&svc->lock);

    if (n == 0) {
        AIRY_FREE(items);
        return AIRY_SUCCESS;
    }

    mem_recent_item_t *shrunk = (mem_recent_item_t *)AIRY_REALLOC(items, n * sizeof(*items));
    *out_items = shrunk ? shrunk : items;
    *out_count = n;
    return AIRY_SUCCESS;
}

void mem_recent_items_free(mem_recent_item_t *items, size_t count)
{
    if (!items)
        return;
    for (size_t i = 0; i < count; i++) {
        AIRY_FREE(items[i].record_id);
        AIRY_FREE(items[i].data);
        AIRY_FREE(items[i].metadata);
    }
    AIRY_FREE(items);
}

void mem_search_hits_free(mem_search_hit_t *hits, size_t count)
{
    if (!hits)
        return;
    for (size_t i = 0; i < count; i++) {
        AIRY_FREE(hits[i].record_id);
    }
    AIRY_FREE(hits);
}

void mem_record_free(mem_record_t *rec)
{
    if (!rec)
        return;
    AIRY_FREE(rec->data);
    AIRY_FREE(rec->metadata);
    rec->data = NULL;
    rec->metadata = NULL;
    rec->len = 0;
}
