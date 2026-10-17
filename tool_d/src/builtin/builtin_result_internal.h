/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_result_internal.h
 * @brief Built-in tool receipt mechanism: the three uniform result shapes.
 *
 *        Every tool entry ends in one of three receipts: a hard failure
 *        that aborts the dispatch with an error code, a soft failure the
 *        calling model can observe and adjust from (AIRY_OK +
 *        success=0 + exit_code=1), or a success carrying output. These
 *        helpers format all three in one place (message buffer sized to
 *        the family's 512-byte convention) so each tool keeps only its
 *        business checks. btool_finish() additionally closes the shared
 *        process-capture exit: adopt the captured output, mirror the
 *        exit code and attach the failure message on non-zero exits.
 */

#ifndef AIRY_RT_TOOL_BUILTIN_RESULT_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_RESULT_INTERNAL_H

#include "builtin/builtin_limits_internal.h"

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void btool_verrf(tool_result_t *res, const char *fmt, va_list ap)
{
    char err[512];
    vsnprintf(err, sizeof(err), fmt, ap);
    res->error = AIRY_STRDUP(err);
}

static inline int btool_fail(tool_result_t *res, int rc, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    btool_verrf(res, fmt, ap);
    va_end(ap);
    return rc;
}

static inline int btool_soft(tool_result_t *res, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    btool_verrf(res, fmt, ap);
    va_end(ap);
    res->success = 0;
    res->exit_code = 1;
    return AIRY_OK;
}

static inline int btool_okf(tool_result_t *res, const char *fmt, ...)
{
    char out[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    res->output = AIRY_STRDUP(out);
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}

static inline int btool_okout(tool_result_t *res, char *out)
{
    res->output = out;
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}

static inline int btool_finish(tool_result_t *res, char *out, int exit_code, const char *what)
{
    res->output = out ? out : AIRY_STRDUP("");
    res->success = (exit_code == 0) ? 1 : 0;
    res->exit_code = exit_code;
    if (exit_code != 0) {
        char err[512];
        snprintf(err, sizeof(err), "%s failed (exit %d)", what, exit_code);
        res->error = AIRY_STRDUP(err);
    }
    return AIRY_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_RESULT_INTERNAL_H */
