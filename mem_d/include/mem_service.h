/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file mem_service.h
 * @brief Public Memory service interface (mem.* namespace).
 *
 * Carries the runtime memory-management logic of the former syscall_router.c
 * (airy_sys_memory_write/search/get/delete), exposed as the service core of
 * the mem_d daemon.
 *
 */

#ifndef AIRY_RT_MEM_SERVICE_H
#define AIRY_RT_MEM_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mem_service mem_service_t;

/** @brief Memory-write parameters. */
typedef struct {
    const void *data;
    size_t len;
    const char *metadata;
} mem_write_request_t;

/** @brief Memory-search result item. */
typedef struct {
    char *record_id;
    float score;
} mem_search_hit_t;

/** @brief Record-read result. */
typedef struct {
    void *data;
    size_t len;
    char *metadata;
} mem_record_t;

/** @brief Recent-record list item（记忆链展示：含内容与时间戳）。 */
typedef struct {
    char *record_id;
    void *data;
    size_t len;
    char *metadata;
    uint64_t created_at;
} mem_recent_item_t;


mem_service_t *mem_service_create(size_t max_records);
void mem_service_destroy(mem_service_t *svc);


/**
 * @brief Write a memory record.
 * @return AIRY_SUCCESS on success; *out_record_id holds the new record ID
 *         (caller AIRY_FREEs)
 */
int mem_service_write(mem_service_t *svc, const mem_write_request_t *req, char **out_record_id);

/**
 * @brief Search memory records (descending relevance).
 * @return AIRY_SUCCESS on success; *out_hits holds the hit array,
 *         *out_count the hit count
 */
int mem_service_search(mem_service_t *svc, const char *query, uint32_t limit,
                       mem_search_hit_t **out_hits, size_t *out_count);

/**
 * @brief Read a memory record.
 * @return AIRY_SUCCESS on success; *out_record holds the record content
 *         (caller frees via mem_record_free)
 */
int mem_service_get(mem_service_t *svc, const char *record_id, mem_record_t *out_record);

/** @brief Delete a memory record. */
int mem_service_delete(mem_service_t *svc, const char *record_id);


size_t mem_service_count(mem_service_t *svc);
void mem_search_hits_free(mem_search_hit_t *hits, size_t count);
void mem_record_free(mem_record_t *rec);

/**
 * @brief 列出最近写入的 N 条记忆记录（记忆链展示）。
 *
 * 按写入顺序取数组尾部（最新）limit 条，倒序返回（新 → 旧），每条
 * 含完整内容与 created_at。limit 为 0 时取默认 10 条。
 * @param svc         Memory service
 * @param limit       最大条数（0 → 10）
 * @param out_items   Malloc'd 数组（按新→旧；用 mem_recent_items_free 释放）
 * @param out_count   返回条数
 * @return AIRY_SUCCESS on success
 */
int mem_service_recent(mem_service_t *svc, uint32_t limit,
                       mem_recent_item_t **out_items, size_t *out_count);
void mem_recent_items_free(mem_recent_item_t *items, size_t count);

/**
 * @brief Ingest a document into a knowledge base (KB).
 *
 * Splits the text into fixed-size chunks (UTF-8 safe boundary) and writes
 * each chunk as a memory record tagged with the KB id (metadata
 * {"kb_id":..,"doc_id":..,"chunk":N}). Records are searchable via
 * mem_service_kb_search and managed (listed/deleted) per KB.
 *
 * @param svc       Memory service
 * @param kb_id     Knowledge-base identifier (required)
 * @param doc_id    Document identifier within the KB (required)
 * @param text      Document text
 * @param len       Text length in bytes
 * @param chunk_size Chunk size in bytes (0 → default 512)
 * @param out_count Written-chunk count (optional)
 * @return AIRY_SUCCESS on success
 */
int mem_service_kb_ingest(mem_service_t *svc, const char *kb_id, const char *doc_id,
                          const void *text, size_t len, size_t chunk_size, size_t *out_count);

/**
 * @brief Search within a knowledge base only.
 * @param svc    Memory service
 * @param kb_id  KB id to filter by
 * @param query  Search query
 * @param limit  Max hits (0 → 10)
 * @param out_hits  Hit array (free with mem_search_hits_free)
 * @param out_count Hit count
 * @return AIRY_SUCCESS on success
 */
int mem_service_kb_search(mem_service_t *svc, const char *kb_id, const char *query,
                          uint32_t limit, mem_search_hit_t **out_hits, size_t *out_count);

/**
 * @brief Delete all records belonging to a knowledge base.
 * @param svc         Memory service
 * @param kb_id       KB id to delete
 * @param out_deleted Deleted-record count (optional)
 * @return AIRY_SUCCESS on success (even when the KB is empty)
 */
int mem_service_kb_delete(mem_service_t *svc, const char *kb_id, size_t *out_deleted);

/**
 * @brief List all knowledge-base ids (deduplicated).
 * @param svc       Memory service
 * @param out_kb_ids Malloc'd string array (free with mem_kb_list_free)
 * @param out_count  KB count
 * @return AIRY_SUCCESS on success
 */
int mem_service_kb_list(mem_service_t *svc, char ***out_kb_ids, size_t *out_count);
void mem_kb_list_free(char **kb_ids, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_MEM_SERVICE_H */
