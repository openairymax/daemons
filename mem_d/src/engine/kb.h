// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file kb.h
 * @brief Knowledge-base service layer (RAG 一等抽象).
 *
 * Public KB API is declared in mem_service.h; this header only exposes the
 * record-level KB predicate that kb.c owns and that the shared search core
 * consumes, so metadata parsing stays in one place.
 */

#ifndef MEM_KB_INTERNAL_H
#define MEM_KB_INTERNAL_H

#include "core/service.h"

/* 记录是否属于 kb_id：解析 metadata 的 kb_id 字段并比较。 */
int mem_rec_in_kb(const mem_record_entry_t *rec, const char *kb_id);

#endif /* MEM_KB_INTERNAL_H */
