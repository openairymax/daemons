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

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_MEM_SERVICE_H */
