// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file scope.c
 * @brief 记忆范围读路径：最近列表与按 conv/kb 过滤的检索。
 *
 * mem.recent 与 mem.search 共用同一套范围谓词（metadata.conv / kb_id）与
 * 写入序倒序扫描语义，故合并为一个读路径文件：内部头 service.h 的 src 面
 * 扇入因此保持 5（G13 拆并裁决：拆后 load ≤ 5）。
 *
 * 最近列表必须全量倒序扫描后再截断——先取最新 limit 条再过滤会让旧会话
 * 占满窗口，本会话反而为空；检索则先过滤后打分，保证 limit 不被范围外
 * 记录占用。
 */

#include "service.h"
#include "airy_memory.h"
#include "airy_types.h"
#include "engine/kb.h"

#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <string.h>

#define MEM_RECENT_DEFAULT_LIMIT 10

/* metadata.<key> 与 val 相等即命中；缺键时按 missing（NULL 表示不匹配）判定。
 * 无 metadata 与解析失败都按缺键处理。 */
static int mem_meta_is(const mem_record_entry_t *rec, const char *key,
                       const char *val, const char *missing)
{
    if (!rec || !val)
        return 0;
    cJSON *root = rec->metadata ? cJSON_Parse(rec->metadata) : NULL;
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    int hit;
    if (cJSON_IsString(item) && item->valuestring)
        hit = strcmp(item->valuestring, val) == 0;
    else
        hit = missing && strcmp(missing, val) == 0;
    if (root)
        cJSON_Delete(root);
    return hit;
}

int mem_rec_in_conv(const mem_record_entry_t *rec, const char *conv)
{
    return mem_meta_is(rec, "conv", conv, NULL);
}

/* 租户键缺省归属 default（与 hall_store 单租户兜底一致）：写入时不带
 * tenant 的记录与显式 tenant=default 等价。 */
int mem_rec_tenant(const mem_record_entry_t *rec, const char *tenant)
{
    return mem_meta_is(rec, "tenant", tenant, "default");
}

int mem_recent_page(mem_service_t *svc, uint32_t limit, const mem_scope_t *scope,
                    int64_t before, mem_recent_item_t **out_items, size_t *out_count,
                    int64_t *out_next, int *out_has_more)
{
    const char *kb_id = scope ? scope->kb_id : NULL;
    const char *conv = scope ? scope->conv : NULL;
    const char *tenant = scope ? scope->tenant : NULL;

    if (!svc || !svc->initialized || !out_items || !out_count)
        return AIRY_ERR_INVALID_PARAM;
    *out_items = NULL;
    *out_count = 0;
    if (out_next)
        *out_next = 0;
    if (out_has_more)
        *out_has_more = 0;

    if (limit == 0)
        limit = MEM_RECENT_DEFAULT_LIMIT;

    airy_mtx_lock(&svc->lock);

    /* 倒序遍历即写入序倒序：写入追加在尾部，删除以 memmove 压实只保持
     * 相对次序，故数组按 rec->seq 严格升序。游标是「写入序号」的排他
     * 上界：只扫描 seq < cursor 的记录；-1 或越界从最新开始（并发删除
     * 后旧游标可能超过最大序号，按最新一页恢复）。用序号而非数组下标，
     * 是为了让页间删除不会把已交付记录重新挤回窗口（X8-01b）。 */
    size_t total = svc->record_count;
    uint64_t newest = svc->write_seq;
    uint64_t cursor = newest;
    if (before >= 0) {
        uint64_t b = (uint64_t)before;
        cursor = b < newest ? b : newest;
    }
    size_t take = total < (size_t)limit ? total : (size_t)limit;
    mem_recent_item_t *items =
        take ? (mem_recent_item_t *)AIRY_CALLOC(take, sizeof(mem_recent_item_t)) : NULL;
    if (take && !items) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    /* 扫描全量而不是只扫最新 limit 条：否则范围外记录会占满窗口，
     * 使更早的匹配记录被静默丢弃（见 mem_service.h 的约定）。
     * 未命中的序号也计入游标推进：下一页从本次最后检查处继续，
     * 因此范围过滤不会造成翻页漏段。 */
    size_t n = 0;
    size_t i = total;
    while (i > 0 && n < limit) {
        i--;
        const mem_record_entry_t *rec = &svc->records[i];
        if (rec->seq >= cursor)
            continue; /* 已交付过的写入序号：跳过，不重复返回 */
        if (!rec->record_id || !rec->data)
            continue;
        if (kb_id && !mem_rec_in_kb(rec, kb_id))
            continue;
        if (conv && !mem_rec_in_conv(rec, conv))
            continue;
        if (tenant && !mem_rec_tenant(rec, tenant))
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

    if (out_next)
        *out_next = i > 0 ? (int64_t)svc->records[i].seq : 0;
    if (out_has_more)
        *out_has_more = i > 0;

    if (n == 0) {
        AIRY_FREE(items);
        return AIRY_SUCCESS;
    }

    mem_recent_item_t *shrunk = (mem_recent_item_t *)AIRY_REALLOC(items, n * sizeof(*items));
    *out_items = shrunk ? shrunk : items;
    *out_count = n;
    return AIRY_SUCCESS;
}

int mem_recent_scope(mem_service_t *svc, uint32_t limit, const mem_scope_t *scope,
                     mem_recent_item_t **out_items, size_t *out_count)
{
    return mem_recent_page(svc, limit, scope, -1, out_items, out_count, NULL, NULL);
}

int mem_recent_scoped(mem_service_t *svc, uint32_t limit, const char *conv,
                      mem_recent_item_t **out_items, size_t *out_count)
{
    mem_scope_t scope = {0};
    scope.conv = conv;
    return mem_recent_scope(svc, limit, &scope, out_items, out_count);
}

int mem_service_recent(mem_service_t *svc, uint32_t limit,
                       mem_recent_item_t **out_items, size_t *out_count)
{
    return mem_recent_scoped(svc, limit, NULL, out_items, out_count);
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

static size_t mem_hits_collect(const mem_rank_t *r, const mem_scope_t *scope,
                               mem_search_hit_t *hits, size_t *vec_used)
{
    const char *kb_id = scope ? scope->kb_id : NULL;
    const char *conv = scope ? scope->conv : NULL;
    const char *tenant = scope ? scope->tenant : NULL;
    size_t n = 0;

    *vec_used = 0;
    for (size_t i = 0; i < r->svc->record_count; i++) {
        const mem_record_entry_t *rec = &r->svc->records[i];
        if (kb_id && !mem_rec_in_kb(rec, kb_id))
            continue;
        if (conv && !mem_rec_in_conv(rec, conv))
            continue;
        if (tenant && !mem_rec_tenant(rec, tenant))
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
    return mem_search_scope(svc, NULL, query, limit, out_hits, out_count);
}

/* 兼容入口：仅按 kb_id 过滤（kb.c 使用）。 */
int mem_service_search_filtered(mem_service_t *svc, const char *kb_id, const char *query,
                                uint32_t limit, mem_search_hit_t **out_hits, size_t *out_count)
{
    mem_scope_t scope = {0};
    scope.kb_id = kb_id;
    return mem_search_scope(svc, &scope, query, limit, out_hits, out_count);
}

int mem_search_scoped(mem_service_t *svc, const char *kb_id, const char *conv,
                      const char *query, uint32_t limit,
                      mem_search_hit_t **out_hits, size_t *out_count)
{
    mem_scope_t scope = {0};
    scope.kb_id = kb_id;
    scope.conv = conv;
    return mem_search_scope(svc, &scope, query, limit, out_hits, out_count);
}

/* 共享检索核心：作用域非空时仅在匹配记录中打分（先过滤后排名，
 * 保证 limit 不被范围外记录占用）。 */
int mem_search_scope(mem_service_t *svc, const mem_scope_t *scope, const char *query,
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
        hit_count = mem_hits_collect(&r, scope, hits, &vec_used);
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
