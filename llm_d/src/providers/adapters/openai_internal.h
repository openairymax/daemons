// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file openai_internal.h
 * @brief OpenAI 适配器内部共享类型与常量（域拆分后跨文件共享）。
 *
 * 域拆分（2026-08-27：原 openai.c 820 行 → 3 文件 + 本头）：
 * - openai.c        生命周期（init/destroy）、非流式 complete、openai_ops
 * - openai_stream.c SSE 流式 completion 与 tool-call 增量累积
 *
 * 令牌桶限流与 429 退避已提升为 core 通用机制（B16-S3）：
 * openai_rate_limiter_t → provider_rate_limiter_t（core/rate_limit.h）。
 * 本头持有 openai 专属常量、ctx 布局与流式声明；对外公共符号
 * （openai_ops）与行为不变。
 */

#ifndef AIRY_RT_LLM_PROVIDERS_OPENAI_INTERNAL_H
#define AIRY_RT_LLM_PROVIDERS_OPENAI_INTERNAL_H

#include "core/adapter.h"
#include "core/rate_limit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPENAI_DEFAULT_BASE "https://api.openai.com/v1"
#define OPENAI_DEFAULT_MODEL "gpt-3.5-turbo"
#define OPENAI_DEFAULT_TPM_TIER2 300000 /* Tokens per minute (Tier 2) */

/* OpenAI 适配器运行时上下文。约定：首字段必须为 provider_base_ctx_t base
 * （provider_base_ctx() 依赖此布局）。 */
typedef struct {
    provider_base_ctx_t base;
    provider_rate_limiter_t rl;
} openai_ctx_t;

/* 流式 completion 定义于 openai_stream.c，openai.c 的 openai_ops 表跨文件
 * 引用（提升为非 static，见本头说明）。 */
int openai_complete_stream(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                           llm_stream_callback_t callback, void *user_data,
                           llm_response_t **out_response);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_PROVIDERS_OPENAI_INTERNAL_H */
