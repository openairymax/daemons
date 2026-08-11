/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file vector.h
 * @brief Memory 服务向量检索内部接口（自研 TF-IDF，无外部依赖）
 *
 * 为 mem_d 提供轻量级向量检索能力：
 * - tokenizer：英文按小写单词切分（可去停用词），中文按单字 + 相邻双字 bigram 切分
 * - 每条记忆记录写入时构建词频向量（TF）缓存于内存，JSONL 持久化保持原文不变，
 *   重启时从 JSONL 重建向量
 * - 全局文档频率（DF）表 + 平滑 IDF，search 时实时计算 TF-IDF 余弦相似度
 */

#ifndef MEM_VECTOR_INTERNAL_H
#define MEM_VECTOR_INTERNAL_H

#include <stddef.h>
#include <stdint.h>


typedef struct {
    char *term;
    float tf;
} mem_vec_term_t;

typedef struct {
    mem_vec_term_t *terms;
    size_t term_count;
    size_t term_capacity;
    size_t total_tokens;
} mem_tfidf_vec_t;


typedef struct {
    char *key;
    size_t df;
    int state;
} mem_df_entry_t;

typedef struct {
    mem_df_entry_t *entries;
    size_t capacity;
    size_t count;
} mem_df_table_t;


/**
 * @brief 对文本 tokenize 并构建词频向量（terms 已去重，tf 为原始计数）
 * @param text 文本（可为 NULL/空，此时输出空向量）
 * @param len 文本长度（字节）
 * @param out 输出向量（调用前无需初始化，成功后由 mem_vec_destroy 释放）
 * @return AIRY_SUCCESS 或错误码
 */
int mem_vec_build(const char *text, size_t len, mem_tfidf_vec_t *out);

/**
 * @brief 释放词频向量资源并置零结构体
 */
void mem_vec_destroy(mem_tfidf_vec_t *vec);

/**
 * @brief 初始化全局文档频率表
 */
int mem_df_init(mem_df_table_t *tbl, size_t capacity);

/**
 * @brief 释放文档频率表资源
 */
void mem_df_destroy(mem_df_table_t *tbl);

/**
 * @brief 文档向量加入全局 DF 表（对每个词项 df+1，首次出现则插入）
 */
void mem_df_add_doc(mem_df_table_t *tbl, const mem_tfidf_vec_t *doc);

/**
 * @brief 文档向量从全局 DF 表移除（对每个词项 df-1，df 归零时删除条目）
 */
void mem_df_remove_doc(mem_df_table_t *tbl, const mem_tfidf_vec_t *doc);

/**
 * @brief 查询与文档的 TF-IDF 余弦相似度 [0,1]
 *
 * IDF 按当前全局 DF 表实时计算（写入后文档集合变化导致权重轻微过期，
 * 实时计算保证检索质量始终正确）。查询/文档向量为空或无可共享词项时返回 0。
 *
 * @param query 查询向量
 * @param doc 文档向量
 * @param df_tbl 全局文档频率表
 * @param doc_count 当前文档总数（N）
 */
float mem_vec_cosine(const mem_tfidf_vec_t *query, const mem_tfidf_vec_t *doc,
                     const mem_df_table_t *df_tbl, size_t doc_count);

/**
 * @brief 英文停用词判断（传入小写词项）
 */
int mem_vec_is_stopword(const char *term);

#endif /* MEM_VECTOR_INTERNAL_H */
