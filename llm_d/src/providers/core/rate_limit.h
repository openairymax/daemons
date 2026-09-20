// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file rate_limit.h
 * @brief provider 域通用限流与重试机制（跨厂商共用）。
 *
 * B16-S3 自 openai 适配器提升为 core 机制（0.1.18 方案表 C）：
 * 原实现仅 openai 一家享有，属机制漏装。限流器状态、默认限流
 * 参数与带退避的重试 HTTP 请求均以 provider 通用形态提供，各
 * 适配器在自己的 ctx 中内嵌 provider_rate_limiter_t 即可获得
 * 同一套 RPM/TPM 令牌桶与 429 退避行为。
 */

#ifndef AIRY_RT_LLM_PROVIDERS_CORE_RATE_LIMIT_H
#define AIRY_RT_LLM_PROVIDERS_CORE_RATE_LIMIT_H

#include "daemon_platform_ext.h"
#include "transport.h"

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROVIDER_RL_DEFAULT_RPM 500 /* Requests per minute */
#define PROVIDER_RL_DEFAULT_TPM 150000 /* Tokens per minute */
#define PROVIDER_RL_MAX_RETRIES 5
#define PROVIDER_RL_BASE_DELAY_MS 1000
#define PROVIDER_RL_MAX_DELAY_MS 60000
#define PROVIDER_RL_JITTER_FACTOR 0.2

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
} provider_rate_limiter_t;

void provider_rl_init(provider_rate_limiter_t *rl);
void provider_rl_destroy(provider_rate_limiter_t *rl);

/* 带 RPM 限流检查与 429/5xx 退避重试的 HTTP POST。base 提供超时与
 * 重试预算，rl 提供限流状态；传输层瞬时故障（DNS/连接/TLS/超时）
 * 的重试仍由 provider_http_post 负责，两层职责正交。 */
int provider_http_request_with_retry(provider_base_ctx_t *base, provider_rate_limiter_t *rl,
                                     const char *url, struct curl_slist *headers,
                                     const char *body, long *out_http_code,
                                     provider_http_resp_t **out_response);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_PROVIDERS_CORE_RATE_LIMIT_H */
