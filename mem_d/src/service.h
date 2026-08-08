// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file service.h
 * @brief Memory 服务内部结构声明
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef MEM_SERVICE_INTERNAL_H
#define MEM_SERVICE_INTERNAL_H

#include "mem_service.h"

#include "vector.h"
#include "emb_client.h"

#include "platform.h"

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* ---------- 内部哈希表（与 syscall_router.c 解耦的独立实现） ---------- */

typedef struct {
    char *key;
    size_t index;
    int occupied;
} mem_hash_entry_t;

typedef struct {
    mem_hash_entry_t *entries;
    size_t capacity;
    size_t count;
} mem_hash_table_t;

/* ---------- 记忆记录条目 ---------- */

typedef struct {
    char *record_id;        /* 记录唯一标识（UUID 风格） */
    void *data;             /* 原始数据 */
    size_t len;             /* 数据长度 */
    char *metadata;         /* JSON 元数据字符串 */
    float score;            /* 当前相关度分数（用于 search 排序缓存） */
    uint64_t created_at;    /* 创建时间戳（ms） */
    mem_tfidf_vec_t vec;    /* TF-IDF 词频向量（内存缓存，写入时构建，JSONL 只存原文） */
    float *emb;             /* embedding 向量（可选增强，NULL 时降级 TF-IDF） */
    size_t emb_dim;         /* embedding 维度 */
} mem_record_entry_t;

struct mem_service {
    mem_record_entry_t *records;
    size_t record_count;
    size_t max_records;
    mem_hash_table_t record_index;
    mem_df_table_t df_table;    /* 全局文档频率（DF）表，供 TF-IDF IDF 计算 */
    float tfidf_weight;         /* 混合融合权重（TF-IDF/embedding 占比，默认 0.6） */
    mem_emb_client_t emb;       /* 可选 embedding 后端客户端 */
    airy_mtx_t lock;
    int initialized;
    char *jsonl_path;       /* JSONL 持久化文件路径（${AIRY_RUNTIME_DIR}/mem.jsonl） */
    FILE *jsonl_append_fp;  /* 追加写入用的 FILE*（保持打开，destroy 时关闭） */
};

#endif /* MEM_SERVICE_INTERNAL_H */
