// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file kb.h
 * @brief Knowledge-base service layer (RAG 一等抽象).
 *
 * Public KB API is declared in mem_service.h; this header is intentionally
 * empty — kb.c includes service.h for the internal record layout and
 * calls mem_service_search_filtered / mem_remove_record_at via internal
 * declarations in service.h.
 */

#ifndef MEM_KB_INTERNAL_H
#define MEM_KB_INTERNAL_H

/* All KB public functions are declared in mem_service.h.
 * This file exists as a conventional placeholder for the KB module. */

#endif /* MEM_KB_INTERNAL_H */
