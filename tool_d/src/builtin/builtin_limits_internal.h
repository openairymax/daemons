/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_limits_internal.h
 * @brief Family prelude and resource guards shared by the built-in tools.
 *
 *        Hosts the family's single-source include prelude: every builtin
 *        translation unit pulls in the memory/error/logging and JSON helper
 *        headers plus the common libc headers through this header, so each
 *        domain header stays down to its own declarations. The resource
 *        guards (output caps, scan depth, shell timeout) live here as the
 *        one place both the runner and the accepting tools read from.
 */

#ifndef AIRY_RT_TOOL_BUILTIN_LIMITS_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_LIMITS_INTERNAL_H

#include "airy_memory.h"
#include "error.h"

#include "builtin/builtin.h"
#include "sandbox/os_sandbox.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <cjson_helpers.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Portable stat suite for the fs tools (family single-sourcing): the
 * recursive walkers classify entries via stat() on every platform, and
 * MSVC needs the S_IS* macros spelled out. */
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sys/stat.h>
#include <sys/types.h>
#define S_ISDIR(m) (((m)&_S_IFDIR) != 0)
#define S_ISREG(m) (((m)&_S_IFREG) != 0)
#endif

#define BUILTIN_OUTPUT_CAP (1U << 20) /* 1MB */
#define BUILTIN_SHELL_TIMEOUT_MS 60000
#define BUILTIN_OUTPUT_DRAIN_MS 1000 /* tail-flush bound after exit */

/* Directory-scan guards shared by the recursive fs tools (fs_grep /
 * fs_glob / fs_list). Every scan runs on a monotonic-deadline budget:
 * on expiry the tool returns the partial result plus a truncation mark
 * instead of holding a pool worker past its declared budget. */
#define BUILTIN_SCAN_MAX_DEPTH 48
#define BUILTIN_SCAN_MAX_FILE_BYTES (4U << 20) /* skip files > 4MB in grep */
#define BUILTIN_LIST_MAX_ENTRIES 10000

#endif /* AIRY_RT_TOOL_BUILTIN_LIMITS_INTERNAL_H */
