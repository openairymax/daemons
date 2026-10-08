/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_exec_internal.h
 * @brief Process-execution built-in tools: runner and entry points.
 *
 *        Owns the shell runner, the shared process-capture mechanism and
 *        the shell/git/web entry points dispatched by tool_builtin_run().
 */

#ifndef AIRY_RT_TOOL_BUILTIN_EXEC_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_EXEC_INTERNAL_H

#include "builtin/builtin_limits_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int builtin_shell_run(const char *cmd, const char *cwd, char **out, int *exit_code,
                      uint32_t timeout_ms, int *out_truncated, const os_sandbox_cfg_t *sandbox);

#ifndef _WIN32
/* Child-side setup callback for builtin_proc_capture: runs in the forked
 * child right before exec and must terminate the child itself (exec* or
 * _exit) -- returning is undefined. ctx is caller-owned and stays valid
 * until the process is reaped. */
typedef void (*builtin_child_fn)(void *ctx);

/* Process-capture mechanism shared by the shell and git runners: forks
 * child_fn with stdout+stderr merged onto one pipe and a controlled stdin
 * (data injected when stdin_data != NULL, otherwise EOF -- the daemon's own
 * stdin is never inherited), enforces timeout_ms, and returns the
 * dynamically-grown output buffer (AIRY_MALLOC; *out_truncated is set once
 * it hits BUILTIN_OUTPUT_CAP). timeout_ms is the resolved wall-clock budget
 * (callers apply their own default before calling). Returns 0 on success,
 * non-zero when fork/pipe/allocation failed. */
int builtin_proc_capture(builtin_child_fn child_fn, void *child_ctx, const char *stdin_data,
                         size_t stdin_len, uint32_t timeout_ms, char **out, int *exit_code,
                         int *out_truncated);
#endif

/* Built-in command/network tool implementations (builtin_shell.c /
 * builtin_git.c / builtin_net.c). Every tool takes a wall-clock budget
 * in ms (0 = no budget). */
int shell_run_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int web_fetch_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int web_search_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);

#ifndef _WIN32
int git_exec_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int git_diff_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
int git_apply_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_EXEC_INTERNAL_H */
