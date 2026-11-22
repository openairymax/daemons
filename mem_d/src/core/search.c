// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file search.c
 * @brief 记忆检索：TF-IDF / 嵌入打分、范围过滤与截断。
 *
 * 检索是 mem_d 唯一的读放大路径，独立成文件以便单独推敲：向量准备在锁外
 * 完成（嵌入是网络调用），记录扫描在锁内，排序与截断在锁外。范围过滤
 * （kb_id / conv）先于打分，保证 limit 不被范围外记录占用。
 */

#include "service.h"
#include "airy_types.h"
#include "engine/kb.h"

#include "svc_logger.h"

#include <string.h>

/* 子串命中密度：查询在正文中出现的次数 × 查询长度 / 正文长度。 */
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

static size_t mem_hits_collect(const mem_rank_t *r, const char *kb_id, const char *conv,
                               mem_search_hit_t *hits, size_t *vec_used)
{
    size_t n = 0;

    *vec_used = 0;
    for (size_t i = 0; i < r->svc->record_count; i++) {
        const mem_record_entry_t *rec = &r->svc->records[i];
        if (kb_id && !mem_rec_in_kb(rec, kb_id))
            continue;
        if (conv && !mem_rec_in_conv(rec, conv))
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
    return mem_service_search_scoped(svc, NULL, NULL, query, limit, out_hits, out_count);
}

/* 兼容入口：仅按 kb_id 过滤（kb.c 使用）。 */
int mem_service_search_filtered(mem_service_t *svc, const char *kb_id, const char *query,
                                uint32_t limit, mem_search_hit_t **out_hits, size_t *out_count)
{
    return mem_service_search_scoped(svc, kb_id, NULL, query, limit, out_hits, out_count);
}

/* 共享检索核心：kb_id / conv 非空时仅在匹配记录中打分（先过滤后排名，
 * 保证 limit 不被范围外记录占用）。 */
int mem_service_search_scoped(mem_service_t *svc, const char *kb_id, const char *conv,
                              const char *query, uint32_t limit,
                              mem_search_hit_t **out_hits, size_t *out_count)
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
        hit_count = mem_hits_collect(&r, kb_id, conv, hits, &vec_used);
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
