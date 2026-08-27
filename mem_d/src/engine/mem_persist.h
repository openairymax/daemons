// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file mem_persist.h
 * @brief JSONL file persistence for mem_service (append / rewrite / load).
 */

#ifndef MEM_PERSIST_H
#define MEM_PERSIST_H

#include "service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Resolve the JSONL data-file path (creates parent directory). */
int  mem_jsonl_path_resolve(char **out_path);

/** Append a single record to the open JSONL file. */
void mem_persist_append_record(mem_service_t *svc, const mem_record_entry_t *rec);

/** Rewrite the full JSONL file atomically (tmp + rename). */
void mem_persist_rewrite_all(mem_service_t *svc);

/** Load existing JSONL records into the service on startup. */
void mem_persist_load_existing(mem_service_t *svc);

#ifdef __cplusplus
}
#endif

#endif /* MEM_PERSIST_H */
