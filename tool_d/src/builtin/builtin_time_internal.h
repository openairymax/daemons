/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_time_internal.h
 * @brief Monotonic deadline helpers for the budgeted built-in tools.
 *
 *        Deliberately depends on <stdint.h> alone: the deadline helpers
 *        are pure arithmetic and must stay usable by any tool without
 *        dragging in the family prelude.
 */

#ifndef AIRY_RT_TOOL_BUILTIN_TIME_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_TIME_INTERNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic deadline helpers: deadline_ms(0) returns "no budget"
 * (UINT64_MAX); deadline_hit() is a cheap compare. */
uint64_t builtin_deadline_ms(uint32_t timeout_ms);
int builtin_deadline_hit(uint64_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_TIME_INTERNAL_H */
