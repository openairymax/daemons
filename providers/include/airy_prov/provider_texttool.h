/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_texttool.h: 文本形态工具调用的补全件（0.1.21 P1-8）
 *
 * 为什么需要：OpenAI 兼容端点以 choices[].message.tool_calls 交付工具调用，
 * 但本地/自托管模型（llama.cpp 未启用 jinja 语法约束、部分 GGUF 模板）会把
 * 调用当作普通文本写在 content 里。此时 tool_calls 为空，工具回路看到的是
 * 「模型只说话不调工具」，表现为空转一轮后直接作答——这是「本地托管模型总
 * 是不准确」的代码级根因之一。
 *
 * 本件只在原生 tool_calls 缺席时补位，不做语义猜测：仅识别显式
 * <tool_call>...</tool_call> 块，块内按 name + arguments/parameters/input/args
 * 取字段。识别到即把内容补成标准 tool_calls 数组，并把 finish_reason 归一为
 * tool_calls，使上层回路按工具轮处理；识别不到保持原样。 */

#ifndef AIRY_PROV_PROVIDER_TEXTTOOL_H
#define AIRY_PROV_PROVIDER_TEXTTOOL_H

#include "llm_service_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 遍历 resp->choices，对「无原生 tool_calls 且 content 含 <tool_call> 块」的
 * choice 补建 tool_calls_json。返回补入的调用条数（0 表示未改动）。
 * resp 为 NULL 时返回 0。 */
int prov_text_tool(llm_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_PROV_PROVIDER_TEXTTOOL_H */
