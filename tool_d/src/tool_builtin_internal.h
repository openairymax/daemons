/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_builtin_internal.h
 * @brief Internal cross-file shared declarations of the built-in tool set
 *        (shared by the functional domains after builtin.c was split).
 */

#ifndef AIRY_RT_TOOL_BUILTIN_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_INTERNAL_H

#include "builtin.h"
#include "os_sandbox.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUILTIN_OUTPUT_CAP (1U << 20) /* 1MB */
#define BUILTIN_SHELL_TIMEOUT_MS 60000
#define BUILTIN_OUTPUT_DRAIN_MS 1000 /* tail-flush bound after exit */

/* Common I/O helpers (builtin.c) */
char *builtin_read_all(FILE *fp, int *out_truncated);

void builtin_append_trunc_mark(char *buf, size_t cap, size_t len, const char *mark);
int builtin_shell_run(const char *cmd, char **out, int *exit_code, uint32_t timeout_ms,
                      int *out_truncated, const os_sandbox_cfg_t *sandbox);

/* Built-in tool implementations (builtin_fs.c / builtin_shell.c /
 * builtin_net.c / builtin_git.c) */
int fs_read_tool(const char *params_json, tool_result_t *res);
int fs_write_tool(const char *params_json, tool_result_t *res);
int fs_list_tool(const char *params_json, tool_result_t *res);
int shell_run_tool(const char *params_json, tool_result_t *res);
int web_fetch_tool(const char *params_json, tool_result_t *res);
int fs_glob_tool(const char *params_json, tool_result_t *res);
int fs_grep_tool(const char *params_json, tool_result_t *res);
int fs_edit_tool(const char *params_json, tool_result_t *res);
int fs_delete_tool(const char *params_json, tool_result_t *res);
int web_search_tool(const char *params_json, tool_result_t *res);

#ifndef _WIN32
int git_exec_tool(const char *params_json, tool_result_t *res);
int git_diff_tool(const char *params_json, tool_result_t *res);
int git_apply_tool(const char *params_json, tool_result_t *res);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_INTERNAL_H */
