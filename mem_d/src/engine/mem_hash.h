// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file mem_hash.h
 * @brief Internal hash table for mem_service record index (djb2, open-addressing).
 */

#ifndef MEM_HASH_H
#define MEM_HASH_H

#include "error.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

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

int      mem_ht_init(mem_hash_table_t *ht, size_t capacity);
void     mem_ht_destroy(mem_hash_table_t *ht);
int      mem_ht_insert(mem_hash_table_t *ht, const char *key, size_t index);
ssize_t  mem_ht_lookup(mem_hash_table_t *ht, const char *key);
void     mem_ht_remove(mem_hash_table_t *ht, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* MEM_HASH_H */
