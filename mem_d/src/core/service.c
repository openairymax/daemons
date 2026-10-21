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
 * - Index is the shared commons hindex (djb2 + tombstones), §130 SSoT
 * - Thread safety: all public interfaces take the lock
 * - Record ID: commons airy_oid atom (32-char hex, no external deps)
 * - Search: own TF-IDF vector cosine similarity (vector.c) fused with
 *   substring scoring weighted 0.6/0.4 (weight configurable via
 *   AIRY_MEM_TFIDF_WEIGHT); optional embedding backend (emb_client.c,
 *   AIRY_MEM_EMBEDDING_URL/KEY) enhances and degrades to TF-IDF on call
 *   failure; records without a vector fall back to pure substring scoring
 *   for backward compatibility
 *
 * Refactored: JSONL persistence → mem_persist.c; KB operations → kb.c.
 * This file owns the core service lifecycle, record CRUD, search, and
 * recent-items logic.
 */

#include "service.h"
#include "airy_types.h"
#include "hindex.h"
#include "engine/kb.h"
#include "engine/mem_persist.h"

#include "svc_logger.h"

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
#define MEM_HASH_LOAD_FACTOR 4 /* capacity = max_records * 4 */
#define MEM_DEFAULT_TFIDF_WEIGHT 0.6f

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

/* 记录整体释放：向量 + 四处堆字段，字段随即归零。调用方保证 rec 非空。 */
static void mem_rec_release(mem_record_entry_t *rec)
{
    mem_vec_destroy(&rec->vec);
    AIRY_FREE(rec->emb);
    rec->emb = NULL;
    rec->emb_dim = 0;
    AIRY_FREE(rec->record_id);
    AIRY_FREE(rec->data);
    AIRY_FREE(rec->metadata);
    rec->record_id = NULL;
    rec->data = NULL;
    rec->metadata = NULL;
}

/* 复制裸内容为 NUL 结尾的堆缓冲；失败返回 NULL。 */
static char *mem_dup_data(const void *data, size_t len)
{
    char *p = (char *)AIRY_MALLOC(len + 1);
    if (!p)
        return NULL;
    __builtin_memcpy(p, data, len);
    p[len] = '\0';
    return p;
}

/* 复制记录内容与 metadata 到调用方（须持有记录锁）。 */
static int mem_rec_copy_out(const mem_record_entry_t *rec, void **out_data, size_t *out_len,
                            char **out_meta)
{
    *out_data = mem_dup_data(rec->data, rec->len);
    if (!*out_data)
        return 0;
    *out_len = rec->len;
    *out_meta = rec->metadata ? AIRY_STRDUP(rec->metadata) : NULL;
    return 1;
}

/* record_id 的活跃下标；不存在或越界返回 -1（须持有记录锁）。 */
static ssize_t mem_rec_find(const mem_service_t *svc, const char *record_id)
{
    ssize_t idx = hindex_get(&svc->record_index, record_id);
    if (idx < 0 || (size_t)idx >= svc->record_count)
        return -1;
    return idx;
}

/* 嵌入增强：网络调用在锁外，回填前按 record_id 重新定位记录。 */
static void mem_rec_embed(mem_service_t *svc, const char *record_id, const char *text)
{
    float *vec = NULL;
    size_t dim = 0;

    if (mem_emb_embed(&svc->emb, text, &vec, &dim) != AIRY_SUCCESS || !vec || dim == 0) {
        AIRY_FREE(vec);
        return;
    }

    airy_mtx_lock(&svc->lock);
    ssize_t idx = mem_rec_find(svc, record_id);
    if (idx >= 0) {
        AIRY_FREE(svc->records[idx].emb);
        svc->records[idx].emb = vec;
        svc->records[idx].emb_dim = dim;
        vec = NULL;
    }
    airy_mtx_unlock(&svc->lock);
    AIRY_FREE(vec);
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

    if (hindex_init(&svc->record_index, max_records * MEM_HASH_LOAD_FACTOR) != AIRY_SUCCESS) {
        AIRY_FREE(svc->records);
        AIRY_FREE(svc);
        return NULL;
    }

    if (mem_df_init(&svc->df_table, max_records * 64) != AIRY_SUCCESS) {
        hindex_free(&svc->record_index);
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
    for (size_t i = 0; i < svc->record_count; i++)
        mem_rec_release(&svc->records[i]);
    AIRY_FREE(svc->records);
    hindex_free(&svc->record_index);
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
    char id_buf[AIRY_OID_STR_MAX];

    airy_oid_str(airy_oid_gen(), id_buf, sizeof(id_buf));
    rec->record_id = AIRY_STRDUP(id_buf);
    rec->data = mem_dup_data(req->data, req->len);
    if (!rec->record_id || !rec->data) {
        mem_rec_release(rec);
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    rec->len = req->len;
    rec->metadata = req->metadata ? AIRY_STRDUP(req->metadata) : NULL;
    rec->created_at = (uint64_t)time(NULL);

    mem_record_build_vector(svc, rec);

    int rc = hindex_put(&svc->record_index, rec->record_id, idx);
    if (rc != AIRY_SUCCESS) {
        mem_rec_release(rec);
        airy_mtx_unlock(&svc->lock);
        return rc;
    }

    *out_record_id = AIRY_STRDUP(rec->record_id);
    svc->record_count++;
    size_t total = svc->record_count;
    mem_persist_append_record(svc, rec);

    /* 解锁前先拷贝内容到本地缓冲：防并发 delete 导致 use-after-free */
    char *copy = mem_emb_should_try(&svc->emb) ? mem_dup_data(rec->data, rec->len) : NULL;
    airy_mtx_unlock(&svc->lock);

    /* Optional embedding enhancement (network call outside the lock) */
    if (copy)
        mem_rec_embed(svc, *out_record_id, copy);
    AIRY_FREE(copy);

    SVC_LOG_DEBUG("Memory write: record_id=%s, len=%zu, total=%lu", *out_record_id, req->len,
                  (unsigned long)total);
    return AIRY_SUCCESS;
}

/* ── Search ──────────────────────────────────────────────────────────── */

/* 一次检索的输入态：预处理好的 TF-IDF 向量、可选嵌入向量及打分对象。 */
typedef struct {
    const mem_service_t *svc;
    const char *query;
    mem_tfidf_vec_t qvec;
    float *emb;
    size_t emb_dim;
    int use_emb;
} mem_rank_t;

/* 向量准备在锁外完成：嵌入是网络调用，不能占住记录锁。 */
static void mem_rank_open(mem_service_t *svc, const char *query, mem_rank_t *r)
{
    AIRY_MEMSET(r, 0, sizeof(*r));
    r->svc = svc;
    r->query = query;
    (void)mem_vec_build(query, strlen(query), &r->qvec);
    if (!mem_emb_should_try(&svc->emb))
        return;
    if (mem_emb_embed(&svc->emb, query, &r->emb, &r->emb_dim) != AIRY_SUCCESS)
        return;
    r->use_emb = r->emb && r->emb_dim > 0;
}

static void mem_rank_close(mem_rank_t *r)
{
    mem_vec_destroy(&r->qvec);
    AIRY_FREE(r->emb);
}

static float mem_rec_vec_score(const mem_rank_t *r, const mem_record_entry_t *rec)
{
    if (r->use_emb && rec->emb && rec->emb_dim == r->emb_dim)
        return mem_emb_cosine(r->emb, rec->emb, r->emb_dim);
    if (rec->vec.term_count > 0)
        return mem_vec_cosine(&r->qvec, &rec->vec, &r->svc->df_table, r->svc->record_count);
    return 0.0f;
}

/* 单条记录打分：向量分与子串分按权重融合，非正值不入榜。 */
static int mem_hit_take(const mem_rank_t *r, const mem_record_entry_t *rec, mem_search_hit_t *hit)
{
    float w = r->svc->tfidf_weight;
    float score = w * mem_rec_vec_score(r, rec) + (1.0f - w) * mem_compute_score(rec, r->query);

    if (score <= 0.0f)
        return 0;
    hit->record_id = AIRY_STRDUP(rec->record_id);
    hit->score = score;
    return 1;
}

static size_t mem_hits_collect(const mem_rank_t *r, const char *kb_id, mem_search_hit_t *hits,
                               size_t *vec_used)
{
    size_t n = 0;

    *vec_used = 0;
    for (size_t i = 0; i < r->svc->record_count; i++) {
        const mem_record_entry_t *rec = &r->svc->records[i];
        if (kb_id && !mem_rec_in_kb(rec, kb_id))
            continue;
        if (rec->vec.term_count > 0)
            (*vec_used)++;
        if (mem_hit_take(r, rec, &hits[n]))
            n++;
    }
    return n;
}

/* 降序排序后只保留前 limit 条，尾部落榜项就地释放。 */
static size_t mem_hits_trim(mem_search_hit_t *hits, size_t n, uint32_t limit)
{
    for (size_t i = 0; i + 1 < n; i++) {
        for (size_t j = 0; j + 1 < n - i; j++) {
            if (hits[j].score < hits[j + 1].score) {
                mem_search_hit_t t = hits[j];
                hits[j] = hits[j + 1];
                hits[j + 1] = t;
            }
        }
    }
    if (n <= limit)
        return n;
    for (size_t i = limit; i < n; i++)
        AIRY_FREE(hits[i].record_id);
    return limit;
}

int mem_service_search(mem_service_t *svc, const char *query, uint32_t limit,
                       mem_search_hit_t **out_hits, size_t *out_count)
{
    return mem_service_search_filtered(svc, NULL, query, limit, out_hits, out_count);
}

/* 共享检索核心：kb_id 非空时仅在属于该知识库的记录中打分。 */
int mem_service_search_filtered(mem_service_t *svc, const char *kb_id, const char *query,
                                uint32_t limit, mem_search_hit_t **out_hits, size_t *out_count)
{
    mem_rank_t r;
    mem_search_hit_t *hits;
    size_t hit_count = 0;
    size_t vec_used = 0;
    int oom;

    if (!svc || !svc->initialized || !query || !out_hits || !out_count)
        return AIRY_ERR_INVALID_PARAM;

    *out_hits = NULL;
    *out_count = 0;
    if (limit == 0)
        limit = 10;

    mem_rank_open(svc, query, &r);

    airy_mtx_lock(&svc->lock);
    hits = svc->record_count ? (mem_search_hit_t *)AIRY_CALLOC(svc->record_count, sizeof(*hits))
                             : NULL;
    oom = !hits && svc->record_count > 0;
    if (!oom)
        hit_count = mem_hits_collect(&r, kb_id, hits, &vec_used);
    airy_mtx_unlock(&svc->lock);

    mem_rank_close(&r);
    if (oom)
        return AIRY_ERR_OUT_OF_MEMORY;

    hit_count = mem_hits_trim(hits, hit_count, limit);
    if (hit_count == 0) {
        AIRY_FREE(hits);
        return AIRY_SUCCESS;
    }

    mem_search_hit_t *shrunk = (mem_search_hit_t *)AIRY_REALLOC(hits, hit_count * sizeof(*hits));
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

    ssize_t idx = mem_rec_find(svc, record_id);
    if (idx < 0) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }

    if (!mem_rec_copy_out(&svc->records[idx], &out_record->data, &out_record->len,
                          &out_record->metadata)) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

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

    hindex_del(&svc->record_index, removed_id);

    mem_df_remove_doc(&svc->df_table, &rec->vec);
    mem_rec_release(rec);

    size_t last = svc->record_count - 1;
    if (idx != last) {
        svc->records[idx] = svc->records[last];
        hindex_put(&svc->record_index, svc->records[idx].record_id, idx);
    }
    __builtin_memset(&svc->records[last], 0, sizeof(mem_record_entry_t));
    svc->record_count--;
}

int mem_service_delete(mem_service_t *svc, const char *record_id)
{
    if (!svc || !svc->initialized || !record_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);

    ssize_t idx = mem_rec_find(svc, record_id);
    if (idx < 0) {
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
        if (!mem_rec_copy_out(rec, &it->data, &it->len, &it->metadata)) {
            AIRY_FREE(it->record_id);
            it->record_id = NULL;
            continue;
        }
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
