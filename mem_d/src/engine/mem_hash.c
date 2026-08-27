// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file mem_hash.c
 * @brief djb2 hash table implementation for mem_service record index.
 *
 * Open-addressing with linear probing; capacity × 3/4 load-factor limit.
 * Extracted from service.c to keep the hash-table code independently
 * testable and to reduce service.c size.
 */

#include "mem_hash.h"
#include "airy_memory.h"
#include <string.h>

static unsigned long mem_hash_fn(const char *str)
{
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

int mem_ht_init(mem_hash_table_t *ht, size_t capacity)
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

void mem_ht_destroy(mem_hash_table_t *ht)
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

int mem_ht_insert(mem_hash_table_t *ht, const char *key, size_t index)
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

ssize_t mem_ht_lookup(mem_hash_table_t *ht, const char *key)
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

void mem_ht_remove(mem_hash_table_t *ht, const char *key)
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
