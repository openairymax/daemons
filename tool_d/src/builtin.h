// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
//
// @file builtin.h
// @brief tool_d 内置基础工具集接口（fs_read / fs_write / fs_list / shell_run）

#ifndef AIRY_RT_TOOL_BUILTIN_H
#define AIRY_RT_TOOL_BUILTIN_H

#include "tool_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 判断 executable 是否为内置工具标记（"builtin:xxx"） */
int tool_builtin_is_builtin(const char *executable);

/* 执行内置工具：tool_id ∈ {fs_read, fs_write, fs_list, shell_run, web_fetch}
 * params_json 为 OpenAI tool_call arguments（JSON 对象字符串）；
 * 结果写入 res（output/error/exit_code/success）。 */
int tool_builtin_run(const char *tool_id, const char *params_json, tool_result_t *res);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_H */
