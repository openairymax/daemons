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

    const char *dir = getenv("AIRY_RUNTIME_DIR");
    if (!dir || !dir[0])
        dir = AIRY_RUNTIME_DIR;

    (void)airy_mkdir_p(dir);

    size_t dir_len = strlen(dir);
    size_t need = dir_len + 1 + strlen(MEM_JSONL_FILENAME) + 1;
    char *path = (char *)AIRY_MALLOC(need);
    if (!path)
        return AIRY_ERR_OUT_OF_MEMORY;
    snprintf(path, need, "%s/%s", dir, MEM_JSONL_FILENAME);
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

    FILE *f = fopen(svc->jsonl_path, "w");
    if (!f) {
        SVC_LOG_WARN("mem_d persist: open rewrite failed (path=%s errno=%d)", svc->jsonl_path,
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
        fprintf(f,
                "{\"record_id\":\"%s\",\"data\":\"%s\",\"metadata\":%s,\"created_at\":%llu,\"len\":"
                "%zu}\n",
                rec->record_id, data_esc, meta, (unsigned long long)rec->created_at, rec->len);
        AIRY_FREE(data_esc);
    }

    if (fflush(f) != 0) {
        SVC_LOG_WARN("mem_d persist: rewrite fflush failed (errno=%d)", errno);
    }
    fclose(f);
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

    mem_persist_append_record(svc, rec);

    airy_mtx_unlock(&svc->lock);

    /* Optional embedding enhancement: make the network call outside the lock
     * (avoid blocking while holding it); on success backfill the vector; on
     * failure emb_client already marks unhealthy and cools down, degrading to
     * TF-IDF automatically */
    if (mem_emb_should_try(&svc->emb)) {
        float *emb_vec = NULL;
        size_t emb_dim = 0;
        if (mem_emb_embed(&svc->emb, (const char *)rec->data, &emb_vec, &emb_dim) == AIRY_SUCCESS &&
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
        AIRY_FREE(emb_vec);
    }

    SVC_LOG_DEBUG("Memory write: record_id=%s, len=%zu, total=%lu", *out_record_id, req->len,
                  (unsigned long)total_writes);
    return AIRY_SUCCESS;
}

int mem_service_search(mem_service_t *svc, const char *query, uint32_t limit,
                       mem_search_hit_t **out_hits, size_t *out_count)
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

    mem_df_remove_doc(&svc->df_table, &svc->records[idx].vec);
    mem_record_free_vector(&svc->records[idx]);

    AIRY_FREE(svc->records[idx].record_id);
    AIRY_FREE(svc->records[idx].data);
    AIRY_FREE(svc->records[idx].metadata);

    size_t last = svc->record_count - 1;
    if ((size_t)idx != last) {
        svc->records[idx] = svc->records[last];

        mem_ht_remove(&svc->record_index, svc->records[idx].record_id);
        mem_ht_insert(&svc->record_index, svc->records[idx].record_id, idx);
    }
    __builtin_memset(&svc->records[last], 0, sizeof(mem_record_entry_t));
    svc->record_count--;

    mem_ht_remove(&svc->record_index, record_id);

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
