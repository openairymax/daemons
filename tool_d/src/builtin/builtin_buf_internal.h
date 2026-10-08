/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_buf_internal.h
 * @brief Output-capture buffer mechanism shared by the process runners.
 *
 *        The shell, git and web tools all capture child/stdout output into
 *        a capped, dynamically-grown buffer. The grow/clamp/truncate-mark
 *        ritual lives here so every runner observes the same cap policy.
 */

#ifndef AIRY_RT_TOOL_BUILTIN_BUF_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_BUF_INTERNAL_H

#include "builtin/builtin_limits_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Output capture buffer mechanism shared by shell/git runners: push()
 * appends a chunk, doubling the buffer up to BUILTIN_OUTPUT_CAP and
 * clamping in place when the cap is hit (*truncated set). Returns 0 when
 * the buffer cannot grow any further (caller stops reading); mark()
 * appends a truncation notice and re-terminates the string. */
int builtin_buf_push(char **buf, size_t *cap, size_t *len, const char *chunk, size_t n,
                     int *truncated);
void builtin_buf_mark(char *buf, size_t cap, size_t *len, const char *mark);

/* Append a one-shot truncation notice after the fact (used by the
 * scanning tools when they bail out on a deadline). */
void builtin_append_trunc_mark(char *buf, size_t cap, size_t len, const char *mark);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_BUF_INTERNAL_H */
