// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file vector.c
 * @brief Memory 服务向量检索实现（自研 TF-IDF，无外部依赖）
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * 设计要点：
 * - tokenizer：英文按小写单词切分（可选去停用词），中文按单字 + 相邻双字 bigram 切分，
 *   其余 UTF-8 多字节字符（emoji 等）与标点作为分隔符
 * - TF 向量缓存：每条记录在写入时构建去重词频向量，存于内存记录条目
 * - DF 表：全局文档频率统计（开放寻址哈希 + 墓碑删除），删除记录时同步维护
 * - IDF：平滑 IDF = ln((N+1)/(df+1)) + 1，保证恒正；search 时按当前全局 DF 实时计算
 * - 余弦相似度：TF-IDF 加权点积 / L2 范数乘积，结果恒在 [0,1]
 */

#include "vector.h"

#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* 最大英文单词长度（超出截断，防止畸形长词拖慢检索） */
#define MEM_VEC_MAX_WORD_LEN 63
/* 向量初始容量与扩容因子 */
#define MEM_VEC_INIT_CAPACITY 8
/* 哈希表状态：空闲 / 活跃 / 墓碑 */
#define MEM_DF_STATE_FREE 0
#define MEM_DF_STATE_USED 1
#define MEM_DF_STATE_TOMB 2

/* ==================== 英文停用词表（小写） ==================== */

static const char *const mem_stopwords[] = {
    "a", "am", "an", "and", "are", "as", "at", "be", "been", "being",
    "but", "by", "can", "could", "did", "do", "does", "for", "from",
    "had", "has", "have", "he", "her", "his", "i", "if", "in", "into",
    "is", "it", "its", "may", "me", "might", "must", "my", "no", "not",
    "of", "on", "or", "our", "she", "should", "so", "than", "that",
    "the", "their", "them", "then", "there", "these", "they", "this",
    "those", "to", "too", "us", "was", "we", "were", "what", "when",
    "where", "which", "who", "will", "with", "would", "you", "your",
    "s", "t", "m", "d", "re", "ll", "ve",
};

int mem_vec_is_stopword(const char *term)
{
    if (!term || !term[0])
        return 0;
    size_t n = sizeof(mem_stopwords) / sizeof(mem_stopwords[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(term, mem_stopwords[i]) == 0)
            return 1;
    }
    return 0;
}

/* ==================== UTF-8 辅助 ==================== */

/* 返回引导字节对应的 UTF-8 序列长度（1-4），非法字节返回 0 */
static int mem_utf8_seq_len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 0;
}

/* 将 len 字节的 UTF-8 序列解码为 Unicode 码点 */
static uint32_t mem_utf8_decode(const unsigned char *s, int len)
{
    switch (len) {
    case 2:
        return ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    case 3:
        return ((uint32_t)(s[0] & 0x0F) << 12) |
               ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
    case 4:
        return ((uint32_t)(s[0] & 0x07) << 18) |
               ((uint32_t)(s[1] & 0x3F) << 12) |
               ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
    default:
        return 0;
    }
}

/* 判断码点是否为 CJK 表意文字（含扩展区） */
static int mem_utf8_is_cjk(uint32_t cp)
{
    return (cp >= 0x3400u && cp <= 0x4DBFu) ||  /* CJK 扩展 A */
           (cp >= 0x4E00u && cp <= 0x9FFFu) ||  /* CJK 统一表意文字 */
           (cp >= 0xF900u && cp <= 0xFAFFu) ||  /* CJK 兼容表意文字 */
           (cp >= 0x20000u && cp <= 0x2FA1Fu);  /* CJK 扩展 B-F */
}

/* ==================== 词频向量 ==================== */

static int mem_vec_ensure_capacity(mem_tfidf_vec_t *v, size_t need)
{
    if (need <= v->term_capacity)
        return AIRY_SUCCESS;
    size_t cap = v->term_capacity ? v->term_capacity : MEM_VEC_INIT_CAPACITY;
    while (cap < need)
        cap *= 2;
    mem_vec_term_t *nt = (mem_vec_term_t *)AIRY_REALLOC(v->terms,
                                                         cap * sizeof(mem_vec_term_t));
    if (!nt)
        return AIRY_ERR_OUT_OF_MEMORY;
    v->terms = nt;
    v->term_capacity = cap;
    return AIRY_SUCCESS;
}

/* 将词项写入向量：已存在则词频+1，否则追加新词项（tf=1） */
static int mem_vec_upsert(mem_tfidf_vec_t *v, const char *term, size_t tlen)
{
    if (!v || !term)
        return AIRY_ERR_INVALID_PARAM;
    for (size_t i = 0; i < v->term_count; i++) {
        if (strlen(v->terms[i].term) == tlen &&
            strncmp(v->terms[i].term, term, tlen) == 0) {
            v->terms[i].tf += 1.0f;
            return AIRY_SUCCESS;
        }
    }
    int rc = mem_vec_ensure_capacity(v, v->term_count + 1);
    if (rc != AIRY_SUCCESS)
        return rc;
    char *copy = (char *)AIRY_MALLOC(tlen + 1);
    if (!copy)
        return AIRY_ERR_OUT_OF_MEMORY;
    AIRY_MEMCPY(copy, term, tlen);
    copy[tlen] = '\0';
    v->terms[v->term_count].term = copy;
    v->terms[v->term_count].tf = 1.0f;
    v->term_count++;
    return AIRY_SUCCESS;
}

int mem_vec_build(const char *text, size_t len, mem_tfidf_vec_t *out)
{
    if (!out)
        return AIRY_ERR_INVALID_PARAM;
    AIRY_MEMSET(out, 0, sizeof(*out));
    if (!text || len == 0)
        return AIRY_SUCCESS;

    char word[MEM_VEC_MAX_WORD_LEN + 1];
    size_t wlen = 0;
    char last_han[5] = {0}; /* 上一个汉字（UTF-8 3 字节 + '\0'），用于 bigram */
    size_t total_tokens = 0;

    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];

        /* 英文/数字：累积为小写单词 */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            if (wlen < MEM_VEC_MAX_WORD_LEN)
                word[wlen++] = (char)((c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c);
            i++;
            continue;
        }

        /* 遇到分隔符：先冲刷已累积的单词 */
        if (wlen > 0) {
            word[wlen] = '\0';
            if (!mem_vec_is_stopword(word)) {
                if (mem_vec_upsert(out, word, wlen) == AIRY_SUCCESS)
                    total_tokens++;
            }
            wlen = 0;
        }

        /* ASCII 分隔符（空格/标点/控制字符）会打断中文连续 bigram */
        if (c < 0x80) {
            last_han[0] = '\0';
            i++;
            continue;
        }

        /* 多字节 UTF-8 序列 */
        int slen = mem_utf8_seq_len(c);
        if (slen < 2 || i + (size_t)slen > len) {
            /* 非法 UTF-8 序列：跳过 1 字节 */
            last_han[0] = '\0';
            i++;
            continue;
        }
        uint32_t cp = mem_utf8_decode((const unsigned char *)text + i, slen);
        if (mem_utf8_is_cjk(cp)) {
            char buf[5];
            AIRY_MEMCPY(buf, text + i, (size_t)slen);
            buf[slen] = '\0';
            /* 单字 */
            if (mem_vec_upsert(out, buf, (size_t)slen) == AIRY_SUCCESS)
                total_tokens++;
            /* 相邻汉字 bigram */
            if (last_han[0]) {
                char bigram[9];
                int n = snprintf(bigram, sizeof(bigram), "%s%s", last_han, buf);
                if (n > 0 &&
                    mem_vec_upsert(out, bigram, (size_t)n) == AIRY_SUCCESS)
                    total_tokens++;
            }
            AIRY_MEMCPY(last_han, buf, (size_t)slen + 1);
        } else {
            /* 非 CJK 多字节字符（emoji 等）作为分隔符 */
            last_han[0] = '\0';
        }
        i += (size_t)slen;
    }

    /* 末尾冲刷残留单词 */
    if (wlen > 0) {
        word[wlen] = '\0';
        if (!mem_vec_is_stopword(word)) {
            if (mem_vec_upsert(out, word, wlen) == AIRY_SUCCESS)
                total_tokens++;
        }
    }

    out->total_tokens = total_tokens;
    return AIRY_SUCCESS;
}

void mem_vec_destroy(mem_tfidf_vec_t *vec)
{
    if (!vec)
        return;
    for (size_t i = 0; i < vec->term_count; i++)
        AIRY_FREE(vec->terms[i].term);
    AIRY_FREE(vec->terms);
    AIRY_MEMSET(vec, 0, sizeof(*vec));
}

/* ==================== 全局文档频率（DF）表 ==================== */

/* djb2 哈希，与 service.c 同算法保证一致性 */
static unsigned long mem_df_hash_fn(const char *str)
{
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

int mem_df_init(mem_df_table_t *tbl, size_t capacity)
{
    if (!tbl || capacity == 0)
        return AIRY_ERR_INVALID_PARAM;
    tbl->entries = (mem_df_entry_t *)AIRY_CALLOC(capacity, sizeof(mem_df_entry_t));
    if (!tbl->entries) {
        tbl->capacity = 0;
        tbl->count = 0;
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    tbl->capacity = capacity;
    tbl->count = 0;
    return AIRY_SUCCESS;
}

void mem_df_destroy(mem_df_table_t *tbl)
{
    if (!tbl || !tbl->entries)
        return;
    for (size_t i = 0; i < tbl->capacity; i++)
        AIRY_FREE(tbl->entries[i].key);
    AIRY_FREE(tbl->entries);
    AIRY_MEMSET(tbl, 0, sizeof(*tbl));
}

size_t mem_df_get(const mem_df_table_t *tbl, const char *term)
{
    if (!tbl || !tbl->entries || !term || tbl->count == 0)
        return 0;
    unsigned long h = mem_df_hash_fn(term) % tbl->capacity;
    for (size_t i = 0; i < tbl->capacity; i++) {
        size_t pos = (h + i) % tbl->capacity;
        if (tbl->entries[pos].state == MEM_DF_STATE_FREE)
            return 0;
        if (tbl->entries[pos].state == MEM_DF_STATE_USED &&
            strcmp(tbl->entries[pos].key, term) == 0)
            return tbl->entries[pos].df;
    }
    return 0;
}

/* 插入新词项（优先复用墓碑槽） */
static int mem_df_insert(mem_df_table_t *tbl, const char *term)
{
    if (tbl->count >= tbl->capacity * 3 / 4)
        return AIRY_ERR_BUSY;

    unsigned long h = mem_df_hash_fn(term) % tbl->capacity;
    size_t tomb = tbl->capacity;
    for (size_t i = 0; i < tbl->capacity; i++) {
        size_t pos = (h + i) % tbl->capacity;
        if (tbl->entries[pos].state == MEM_DF_STATE_FREE) {
            /* 无墓碑则用空闲槽；有墓碑则复用更早找到的墓碑槽 */
            if (tomb == tbl->capacity)
                tomb = pos;
            break;
        }
        if (tbl->entries[pos].state == MEM_DF_STATE_TOMB) {
            if (tomb == tbl->capacity)
                tomb = pos;
            continue;
        }
        if (strcmp(tbl->entries[pos].key, term) == 0)
            return AIRY_ERR_ALREADY_EXISTS;
    }
    if (tomb == tbl->capacity)
        return AIRY_ERR_BUSY;

    char *k = AIRY_STRDUP(term);
    if (!k)
        return AIRY_ERR_OUT_OF_MEMORY;
    tbl->entries[tomb].key = k;
    tbl->entries[tomb].df = 1;
    tbl->entries[tomb].state = MEM_DF_STATE_USED;
    tbl->count++;
    return AIRY_SUCCESS;
}

static int mem_df_add(mem_df_table_t *tbl, const char *term)
{
    if (!tbl || !tbl->entries || !term)
        return AIRY_ERR_INVALID_PARAM;
    unsigned long h = mem_df_hash_fn(term) % tbl->capacity;
    for (size_t i = 0; i < tbl->capacity; i++) {
        size_t pos = (h + i) % tbl->capacity;
        if (tbl->entries[pos].state == MEM_DF_STATE_FREE)
            break;
        if (tbl->entries[pos].state == MEM_DF_STATE_USED &&
            strcmp(tbl->entries[pos].key, term) == 0) {
            tbl->entries[pos].df++;
            return AIRY_SUCCESS;
        }
    }
    return mem_df_insert(tbl, term);
}

static void mem_df_remove(mem_df_table_t *tbl, const char *term)
{
    if (!tbl || !tbl->entries || !term || tbl->count == 0)
        return;
    unsigned long h = mem_df_hash_fn(term) % tbl->capacity;
    for (size_t i = 0; i < tbl->capacity; i++) {
        size_t pos = (h + i) % tbl->capacity;
        if (tbl->entries[pos].state == MEM_DF_STATE_FREE)
            return;
        if (tbl->entries[pos].state == MEM_DF_STATE_USED &&
            strcmp(tbl->entries[pos].key, term) == 0) {
            if (tbl->entries[pos].df > 1) {
                tbl->entries[pos].df--;
            } else {
                /* df 归零：释放词项并标记墓碑，保持线性探测正确性 */
                AIRY_FREE(tbl->entries[pos].key);
                tbl->entries[pos].key = NULL;
                tbl->entries[pos].df = 0;
                tbl->entries[pos].state = MEM_DF_STATE_TOMB;
                tbl->count--;
            }
            return;
        }
    }
}

void mem_df_add_doc(mem_df_table_t *tbl, const mem_tfidf_vec_t *doc)
{
    if (!tbl || !doc)
        return;
    for (size_t i = 0; i < doc->term_count; i++) {
        int rc = mem_df_add(tbl, doc->terms[i].term);
        if (rc == AIRY_ERR_OUT_OF_MEMORY) {
            SVC_LOG_WARN("mem_d df: out of memory while tracking term");
            break;
        }
        /* BUSY（表满）仅影响 DF 统计精度，不阻断检索 */
    }
}

void mem_df_remove_doc(mem_df_table_t *tbl, const mem_tfidf_vec_t *doc)
{
    if (!tbl || !doc)
        return;
    for (size_t i = 0; i < doc->term_count; i++)
        mem_df_remove(tbl, doc->terms[i].term);
}

/* ==================== TF-IDF 余弦相似度 ==================== */

/* 平滑 IDF：idf = ln((N+1)/(df+1)) + 1，恒为正且 df<=N 时 idf>=1 */
static double mem_idf(size_t df, size_t doc_count)
{
    if (doc_count == 0)
        return 0.0;
    return log(((double)doc_count + 1.0) / ((double)df + 1.0)) + 1.0;
}

float mem_vec_cosine(const mem_tfidf_vec_t *query, const mem_tfidf_vec_t *doc,
                     const mem_df_table_t *df_tbl, size_t doc_count)
{
    if (!query || !doc || !df_tbl)
        return 0.0f;
    if (query->term_count == 0 || doc->term_count == 0)
        return 0.0f;

    double dot = 0.0;
    double q_norm = 0.0;

    /* 遍历查询词项，累计点积与查询范数 */
    for (size_t i = 0; i < query->term_count; i++) {
        double idf = mem_idf(mem_df_get(df_tbl, query->terms[i].term), doc_count);
        double wq = (double)query->terms[i].tf * idf;
        q_norm += wq * wq;

        /* 在文档向量中查找该词项（线性搜索，词项数量通常 < 100） */
        for (size_t j = 0; j < doc->term_count; j++) {
            if (strcmp(query->terms[i].term, doc->terms[j].term) == 0) {
                double wd = (double)doc->terms[j].tf * idf;
                dot += wq * wd;
                break;
            }
        }
    }

    if (q_norm == 0.0 || dot <= 0.0)
        return 0.0f;

    /* 计算文档向量范数（含文档独有词项） */
    double d_norm = 0.0;
    for (size_t j = 0; j < doc->term_count; j++) {
        double idf = mem_idf(mem_df_get(df_tbl, doc->terms[j].term), doc_count);
        double wd = (double)doc->terms[j].tf * idf;
        d_norm += wd * wd;
    }
    if (d_norm == 0.0)
        return 0.0f;

    double cos = dot / (sqrt(q_norm) * sqrt(d_norm));
    if (cos < 0.0)
        cos = 0.0;
    if (cos > 1.0)
        cos = 1.0;
    return (float)cos;
}
