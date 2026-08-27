// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file service.c
 * @brief Memory service implementation: lifecycle, CRUD, search, recent.
 *
 * Extracted from g_runtime.records[] logic in gateway/src/utils/syscall/syscall_router.c
 * and refactored into a standalone, self-contained service module. The
 * mem_d daemon holds a mem_service_t instance and exposes the mem.*
 * namespace over a Unix socket.
 *
 * Design notes:
 * - Own hash table (djb2, now in mem_hash.c)
 * - Thread safety: all public interfaces take the lock
 * - Record ID: 32-char hex (timestamp + counter, no external deps)
 * - Search: own TF-IDF vector cosine similarity (vector.c) fused with
 *   substring scoring weighted 0.6/0.4 (weight configurable via
 *   AIRY_MEM_TFIDF_WEIGHT); optional embedding backend (emb_client.c,
 *   AIRY_MEM_EMBEDDING_URL/KEY) enhances and degrades to TF-IDF on call
 *   failure; records without a vector fall back to pure substring scoring
 *   for backward compatibility
 *
 * Refactored: hash table → mem_hash.c; JSONL persistence → mem_persist.c;
 * KB operations → kb.c.  This file owns the core service lifecycle,
 * record CRUD, search, and recent-items logic.
 */

#include "service.h"
#include "mem_hash.h"
#include "mem_persist.h"

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
#define MEM_DEFAULT_TFIDF_WEIGHT 0.6f

/* ── Record ID generation ────────────────────────────────────────────── */

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

/* ── Scoring helpers ─────────────────────────────────────────────────── */

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

/* ── Vector build / free ─────────────────────────────────────────────── */

void mem_record_build_vector(mem_service_t *svc, mem_record_entry_t *rec)
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

void mem_record_free_vector(mem_record_entry_t *rec)
{
    if (!rec)
        return;
    mem_vec_destroy(&rec->vec);
    AIRY_FREE(rec->emb);
    rec->emb = NULL;
    rec->emb_dim = 0;
}

/* ── Service lifecycle ───────────────────────────────────────────────── */

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

/* ── mem_service_write ───────────────────────────────────────────────── */

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
    (void)total_writes;

    mem_persist_append_record(svc, rec);

    /* 解锁前先拷贝内容到本地缓冲：防并发 delete 导致 use-after-free */
    char *data_copy = NULL;
    if (mem_emb_should_try(&svc->emb) && rec->data && rec->len > 0) {
        data_copy = (char *)AIRY_MALLOC(rec->len + 1);
        if (data_copy) {
            __builtin_memcpy(data_copy, rec->data, rec->len);
            data_copy[rec->len] = '\0';
        }
    }

    airy_mtx_unlock(&svc->lock);

    /* Optional embedding enhancement (network call outside the lock) */
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

/* ── Search ──────────────────────────────────────────────────────────── */

int mem_service_search(mem_service_t *svc, const char *query, uint32_t limit,
                       mem_search_hit_t **out_hits, size_t *out_count)
{
    return mem_service_search_filtered(svc, NULL, query, limit, out_hits, out_count);
}

/* 共享检索核心：kb_id 非空时仅在属于该知识库的记录中打分。 */
int mem_service_search_filtered(mem_service_t *svc, const char *kb_id, const char *query,
                                uint32_t limit, mem_search_hit_t **out_hits, size_t *out_count)
{
    if (!svc || !svc->initialized || !query || !out_hits || !out_count)
        return AIRY_ERR_INVALID_PARAM;

    *out_hits = NULL;
    *out_count = 0;

    if (limit == 0)
        limit = 10;

    /* Prepare outside the lock (network calls do not hold the lock) */
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

        if (kb_id) {
            /* KB filter: inline check via metadata parse */
            const char *rec_kb = NULL;
            if (rec->metadata && rec->metadata[0]) {
                cJSON *root = cJSON_Parse(rec->metadata);
                if (root) {
                    cJSON *kb = cJSON_GetObjectItem(root, "kb_id");
                    if (cJSON_IsString(kb) && kb->valuestring)
                        rec_kb = kb->valuestring;
                    /* Note: we can't use mem_rec_kb_id here (it's in kb.c
                     * with a static buffer); instead we compare inline. */
                    if (rec_kb && strcmp(rec_kb, kb_id) != 0)
                        rec_kb = NULL; /* force skip */
                    cJSON_Delete(root);
                }
            }
            if (!rec_kb)
                continue;
        }

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

    /* Sort by score descending, then truncate to limit */
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

    mem_search_hit_t *shrunk = NULL;
    if (hit_count <= SIZE_MAX / sizeof(mem_search_hit_t)) {
        shrunk = (mem_search_hit_t *)AIRY_REALLOC(hits, hit_count * sizeof(mem_search_hit_t));
    }
    *out_hits = shrunk ? shrunk : hits;
    *out_count = hit_count;

    SVC_LOG_DEBUG("Memory search: query='%s', hits=%zu (vec_used=%zu)", query, hit_count, vec_used);
    return AIRY_SUCCESS;
}

/* ── Get / Delete / Count ────────────────────────────────────────────── */

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
 * 维护 DF 表与 record_index 哈希。
 */
void mem_remove_record_at(mem_service_t *svc, size_t idx)
{
    mem_record_entry_t *rec = &svc->records[idx];
    const char *removed_id = rec->record_id;

    mem_ht_remove(&svc->record_index, removed_id);

    mem_df_remove_doc(&svc->df_table, &rec->vec);
    mem_record_free_vector(rec);

    AIRY_FREE(rec->record_id);
    AIRY_FREE(rec->data);
    AIRY_FREE(rec->metadata);

    size_t last = svc->record_count - 1;
    if (idx != last) {
        svc->records[idx] = svc->records[last];
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

/* ── Recent items ────────────────────────────────────────────────────── */

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
    mem_recent_item_t *items =
        (mem_recent_item_t *)AIRY_CALLOC(take, sizeof(mem_recent_item_t));
    if (!items) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t n = 0;
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

/* ── Free helpers ────────────────────────────────────────────────────── */

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
