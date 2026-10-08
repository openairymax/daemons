/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_fs_internal.h
 * @brief Filesystem built-in tools: shared helpers and tool entry points.
 *
 *        Groups the path-confine, parameter-extraction and recursive-scan
 *        helpers that the fs tool family shares, together with the entry
 *        points dispatched by tool_builtin_run(). Kept apart from the
 *        shell/git/net/maths domains so each family only sees its own
 *        surface.
 */

#ifndef AIRY_RT_TOOL_BUILTIN_FS_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_FS_INTERNAL_H

#include "builtin/builtin_limits_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Directories never worth scanning (VC metadata, package stores, build
 * output). Shared by fs_grep and fs_glob so both stay consistent. */
int builtin_scan_noise_dir(const char *name);

/* Confine a file-tool path into the workspace sandbox (T16). Wraps
 * os_sandbox_fs_confine() with a uniform escape error for tool results.
 * Returns AIRY_OK with resolved filled on success; otherwise sets
 * res->error and returns AIRY_ERR_PERMISSION_DENIED. */
int builtin_fs_confine(const char *orig_path, int for_write, char *resolved, size_t resolved_cap,
                       tool_result_t *res);

/* Common I/O helper (builtin.c) */
char *builtin_read_all(FILE *fp, int *out_truncated);

/* String parameter extraction ritual shared by the tool entry points:
 * fetch name's string value and require it non-empty. Returns AIRY_OK
 * with *out pointing at the value; on missing/empty sets res->error
 * and returns AIRY_ERR_INVALID_PARAM. */
int builtin_str_param(const cJSON *root, const char *name, const char **out, tool_result_t *res);

/* Glob wildcard segment matcher shared by builtin_fs_glob.c and
 * builtin_fs_grep.c (supports * and ? within one path segment) */
int builtin_glob_seg_match(const char *pat, const char *str);

/* Built-in filesystem tool implementations (builtin_fs.c /
 * builtin_fs_dir.c / builtin_fs_glob.c / builtin_fs_grep.c). Every tool
 * takes a wall-clock budget in ms (0 = no budget); scanning tools bail
 * out with a truncation mark on expiry, others may ignore it. */
int fs_read_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int fs_write_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int fs_list_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int fs_glob_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int fs_grep_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int fs_edit_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int fs_delete_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_FS_INTERNAL_H */
