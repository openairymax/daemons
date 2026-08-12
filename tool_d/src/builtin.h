/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @file builtin.h */


#ifndef AIRY_RT_TOOL_BUILTIN_H
#define AIRY_RT_TOOL_BUILTIN_H

#include "tool_service.h"

#ifdef __cplusplus
extern "C" {
#endif


int tool_builtin_is_builtin(const char *executable);

/* Execute a built-in tool: tool_id in {fs_read, fs_write, fs_list,
 * shell_run, web_fetch}. params_json is the OpenAI tool_call arguments
 * (JSON object string); the result is written to res
 * (output/error/exit_code/success). */
int tool_builtin_run(const char *tool_id, const char *params_json, tool_result_t *res);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_H */
