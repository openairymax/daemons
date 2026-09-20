// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file toolstream.h
 * @brief Provider 域流式响应装配 SSoT（B16-S3 c5/c6：三份同构收敛）。
 *
 * OpenAI 兼容流式协议（openai/deepseek/local）以 {index, id?} →
 * {index, function.{name?, arguments}} 分片下发 tool_calls 增量；本模块
 * 按 index 装配槽位，流末装配完整数组并发 RS 'T' 控制帧。适配层禁止
 * 自持副本，只在 on_chunk 内调用 provider_tool_delta 消费 delta。
 *
 * c6 扩展：openai 家族的流式累积器（content/reasoning 增量、id/model/
 * created 抽取、finish_reason 归一、usage 累计）与流末响应装配同为三份
 * 逐字同构，收敛为 provider_stream_acc_t 四件套；适配层只需
 * init → 注册 provider_openai_on_chunk → take/free 两行。
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

/* OpenAI 兼容家族流式累积器：openai/deepseek/local 三份逐字同构的
 * on_chunk 状态（content/reasoning 增量转发与累计、id/model/created
 * 首值锁定、finish_reason 归一、usage 尾 chunk 累计）与流末响应装配。 */
typedef struct {
    llm_stream_callback_t user_cb;
    void *user_data;
    char *acc_content;
    size_t acc_cap;
    size_t acc_len;
    char *acc_reasoning;
    size_t acc_reasoning_cap;
    size_t acc_reasoning_len;
    char *resp_id;
    char *resp_model;
    uint64_t resp_created;
    char *finish_reason;
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;
    uint32_t reasoning_tokens;
    provider_tool_acc_t tools;
} provider_stream_acc_t;

/* 清零并预分配 content 缓冲（4096 起步）。 */
void provider_stream_acc_init(provider_stream_acc_t *acc, llm_stream_callback_t cb, void *ud);

/* 行协议 on_chunk（provider_http_post_stream 回调形状）：消费一条
 * data: JSON 载荷。本地端点不产 reasoning_content 时相应分支自然旁路。 */
int provider_openai_on_chunk(const char *json_line, void *userdata);

/* 流末装配响应并转移缓冲所有权；acc 内已转移指针被复位。 */
llm_response_t *provider_openai_stream_take(provider_stream_acc_t *acc);

/* 释放全部累积缓冲（成功装配后仍持未转移残片或失败路径均安全）。 */
void provider_stream_acc_free(provider_stream_acc_t *acc);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_TOOLSTREAM_H */
