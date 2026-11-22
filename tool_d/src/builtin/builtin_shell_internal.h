/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_shell_internal.h
 * @brief Shell runner contract shared by the command, git, verify and
 *        network tools.
 *
 *        Kept apart from builtin_exec_internal.h so tools that only need
 *        to run a command do not pull the process-capture contract
 *        (child callback plus piped spawn) into their include graph.
 */

#ifndef AIRY_RT_TOOL_BUILTIN_SHELL_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_SHELL_INTERNAL_H

#include "sandbox/os_sandbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 运行 shell 命令：timeout_ms 为 0 表示无预算；strict 非零时禁用宽松兜底。
 * 输出由 AIRY_MALLOC 分配，*out_truncated 标记是否触顶 BUILTIN_OUTPUT_CAP。 */
int builtin_shell_run(const char *cmd, const char *cwd, char **out, int *exit_code,
                      uint32_t timeout_ms, int *out_truncated, const os_sandbox_cfg_t *sandbox,
                      int strict);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_SHELL_INTERNAL_H */
