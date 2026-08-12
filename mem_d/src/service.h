/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service.h
 * @brief Memory service internal structure declarations.
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


typedef struct {
    char *record_id;
    void *data;
    size_t len;
    char *metadata;
    float score;
    uint64_t created_at;
    mem_tfidf_vec_t vec;
    float *emb;
    size_t emb_dim;
} mem_record_entry_t;

struct mem_service {
    mem_record_entry_t *records;
    size_t record_count;
    size_t max_records;
    mem_hash_table_t record_index;
    mem_df_table_t df_table;
    float tfidf_weight;
    mem_emb_client_t emb;
    airy_mtx_t lock;
    int initialized;
    char *jsonl_path;
    FILE *jsonl_append_fp;
};

#endif /* MEM_SERVICE_INTERNAL_H */
