// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file openai_internal.h
 * @brief OpenAI 适配器内部共享类型与常量（域拆分后跨文件共享）。
 *
 * 域拆分（2026-08-27：原 openai.c 820 行 → 3 文件 + 本头）：
 * - openai.c            生命周期（init/destroy）、非流式 complete、openai_ops
 * - openai_rate_limit.c 令牌桶限流（RPM/TPM）、429 退避、带重试的 HTTP 请求
 * - openai_stream.c     SSE 流式 completion 与 tool-call 增量累积
 *
 * 本头持有三文件共享的 ctx 布局、限流器结构与 openai_http_request_with_retry
 * 声明；对外公共符号（openai_ops）与行为不变。
 */

#ifndef AIRY_RT_LLM_PROVIDERS_OPENAI_INTERNAL_H
#define AIRY_RT_LLM_PROVIDERS_OPENAI_INTERNAL_H

#include "daemon_platform_ext.h"
#include "provider.h"

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENAI_DEFAULT_BASE "https://api.openai.com/v1"
#define OPENAI_DEFAULT_MODEL "gpt-3.5-turbo"

#define OPENAI_DEFAULT_RPM 500 /* Requests per minute */
#define OPENAI_DEFAULT_TPM 150000 /* Tokens per minute (Tier 1) */
#define OPENAI_DEFAULT_TPM_TIER2 300000 /* Tokens per minute (Tier 2) */
#define OPENAI_MAX_RETRIES 5
#define OPENAI_BASE_DELAY_MS 1000
#define OPENAI_MAX_DELAY_MS 60000
#define OPENAI_JITTER_FACTOR 0.2

/* 令牌桶限流器：RPM/TPM 窗口计数 + HTTP 429 退避状态 */
typedef struct {
    airy_mtx_t lock;
    time_t rpm_window_start;
    int rpm_count;
    int rpm_limit;
    long tpm_count;
    time_t tpm_window_start;
    long tpm_limit;
    time_t last_429_time;
    int retry_after_sec;
    int consecutive_429s;
} openai_rate_limiter_t;

/* OpenAI 适配器运行时上下文。约定：首字段必须为 provider_base_ctx_t base
 * （provider_base_ctx() 依赖此布局）。 */
typedef struct {
    provider_base_ctx_t base;
    openai_rate_limiter_t rl;
} openai_ctx_t;

void openai_rl_init(openai_rate_limiter_t *rl);
void openai_rl_destroy(openai_rate_limiter_t *rl);

int openai_http_request_with_retry(openai_ctx_t *ctx, const char *url,
                                   struct curl_slist *headers, const char *body,
                                   long *out_http_code, provider_http_resp_t **out_response);

/* 流式 completion 定义于 openai_stream.c，openai.c 的 openai_ops 表跨文件
 * 引用（提升为非 static，见 openai_internal.h 说明）。 */
int openai_complete_stream(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                           llm_stream_callback_t callback, void *user_data,
                           llm_response_t **out_response);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_PROVIDERS_OPENAI_INTERNAL_H */
