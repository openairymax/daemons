// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file toolstream.h
 * @brief Provider 域流式 tool_calls 装配 SSoT（B16-S3 c5：三份同构收敛）。
 *
 * OpenAI 兼容流式协议（openai/deepseek/local）以 {index, id?} →
 * {index, function.{name?, arguments}} 分片下发 tool_calls 增量；本模块
 * 按 index 装配槽位，流末装配完整数组并发 RS 'T' 控制帧。适配层禁止
 * 自持副本，只在 on_chunk 内调用 provider_tool_delta 消费 delta。
 */

#ifndef LLM_D_PROVIDERS_CORE_TOOLSTREAM_H
#define LLM_D_PROVIDERS_CORE_TOOLSTREAM_H

#include "llm_service_types.h"
#include "airy_llm_stream.h"
#include "airy_memory.h"

#include <cjson/cJSON.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROVIDER_TOOL_MAX 16
#define PROVIDER_TOOL_STR_MAX 128

typedef struct {
    int index;
    char id[PROVIDER_TOOL_STR_MAX];
    char name[PROVIDER_TOOL_STR_MAX];
    char *args;
    size_t args_len;
    size_t args_cap;
} provider_tool_slot_t;

typedef struct {
    provider_tool_slot_t slot[PROVIDER_TOOL_MAX];
    size_t count;
} provider_tool_acc_t;

/* 消费 choices[].delta：抽取 tool_calls 增量分片并按 index 装配。 */
void provider_tool_delta(provider_tool_acc_t *acc, cJSON *delta);

/* 装配完整 tool_calls JSON 数组（OpenAI 非流式响应形状）；返回值由调用方 free。 */
char *provider_tool_json(const provider_tool_acc_t *acc);

/* 流末发射：发 RS 'T' 控制帧并落入 resp->choices[0].tool_calls_json；
 * resp 无空槽位时由本函数释放 JSON。空槽位时 no-op。 */
void provider_tool_flush(provider_tool_acc_t *acc, llm_response_t *resp,
                         llm_stream_callback_t cb, void *ud);

/* 释放槽位参数缓冲并复位（调用后 acc 可安全复用）。 */
void provider_tool_free(provider_tool_acc_t *acc);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_TOOLSTREAM_H */
