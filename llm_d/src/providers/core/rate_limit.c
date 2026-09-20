// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file rate_limit.c
 * @brief provider 域通用令牌桶限流与重试。
 *
 * 自 openai 适配器提升为 core 机制（B16-S3，0.1.18 方案表 C）：
 * - RPM/TPM 令牌桶窗口计数
 * - HTTP 429 检测与 Retry-After 解析
 * - 指数退避 + 抖动
 * - provider_http_request_with_retry（限流 + 429/5xx 退避的 HTTP POST）
 *
 * provider_rl_init / provider_rl_destroy / provider_http_request_with_retry
 * 由各适配器跨文件调用，声明见 rate_limit.h；其余为文件内 static。
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "error.h"
#include "provider.h"
#include "rate_limit.h"
#include "svc_logger.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void provider_rl_init(provider_rate_limiter_t *rl)
{
    airy_mtx_init(&rl->lock);
    rl->rpm_window_start = time(NULL);
    rl->rpm_count = 0;
    rl->rpm_limit = PROVIDER_RL_DEFAULT_RPM;
    rl->tpm_count = 0;
    rl->tpm_window_start = time(NULL);
    rl->tpm_limit = PROVIDER_RL_DEFAULT_TPM;
    rl->last_429_time = 0;
    rl->retry_after_sec = 0;
    rl->consecutive_429s = 0;
}

void provider_rl_destroy(provider_rate_limiter_t *rl)
{
    airy_mtx_destroy(&rl->lock);
}

static int rl_check_rpm(provider_rate_limiter_t *rl)
{
    time_t now = time(NULL);
    airy_mtx_lock(&rl->lock);

    if (now - rl->rpm_window_start >= 60) {
        rl->rpm_count = 0;
        rl->rpm_window_start = now;
    }

    if (rl->rpm_count >= rl->rpm_limit) {
        airy_mtx_unlock(&rl->lock);
        return AIRY_ERR_LLM_RATE_LIMIT;
    }

    rl->rpm_count++;
    airy_mtx_unlock(&rl->lock);
    return 0;
}

static int __attribute__((unused)) rl_check_tpm(provider_rate_limiter_t *rl, int tokens)
{
    time_t now = time(NULL);
    airy_mtx_lock(&rl->lock);

    if (now - rl->tpm_window_start >= 60) {
        rl->tpm_count = 0;
        rl->tpm_window_start = now;
    }

    if (rl->tpm_count + tokens > rl->tpm_limit) {
        airy_mtx_unlock(&rl->lock);
        return AIRY_ERR_LLM_RATE_LIMIT;
    }

    rl->tpm_count += tokens;
    airy_mtx_unlock(&rl->lock);
    return 0;
}

static void rl_record_429(provider_rate_limiter_t *rl, int retry_after)
{
    time_t now = time(NULL);
    airy_mtx_lock(&rl->lock);

    rl->last_429_time = now;
    rl->consecutive_429s++;
    if (retry_after > 0) {
        rl->retry_after_sec = retry_after;
    } else {
        rl->retry_after_sec = 0;
    }

    airy_mtx_unlock(&rl->lock);
}

static int rl_get_wait_ms(provider_rate_limiter_t *rl, int attempt)
{
    airy_mtx_lock(&rl->lock);

    int wait_ms;

    if (rl->retry_after_sec > 0) {
        wait_ms = rl->retry_after_sec * 1000;
        rl->retry_after_sec = 0;
    } else {
        int base_delay = PROVIDER_RL_BASE_DELAY_MS << attempt;
        if (base_delay > PROVIDER_RL_MAX_DELAY_MS)
            base_delay = PROVIDER_RL_MAX_DELAY_MS;

        double jitter =
            ((double)airy_random_uint32(0, 99) / 100.0) * base_delay * PROVIDER_RL_JITTER_FACTOR;
        wait_ms = (int)((double)base_delay + jitter);
    }

    airy_mtx_unlock(&rl->lock);
    return wait_ms;
}

static void rl_reset_429(provider_rate_limiter_t *rl)
{
    airy_mtx_lock(&rl->lock);
    rl->consecutive_429s = 0;
    rl->retry_after_sec = 0;
    airy_mtx_unlock(&rl->lock);
}

static int parse_retry_after(const char *headers_data)
{
    if (!headers_data)
        return 0;

    const char *retry_ptr = strstr(headers_data, "retry-after:");
    if (!retry_ptr)
        retry_ptr = strstr(headers_data, "Retry-After:");
    if (!retry_ptr)
        return 0;

    retry_ptr = strchr(retry_ptr, ':');
    if (!retry_ptr)
        return 0;
    retry_ptr++;

    while (*retry_ptr == ' ' || *retry_ptr == '\t')
        retry_ptr++;

    long seconds = strtol(retry_ptr, NULL, 10);
    if (seconds <= 0)
        return 0;
    if (seconds > 300)
        seconds = 300;

    return (int)seconds;
}

int provider_http_request_with_retry(provider_base_ctx_t *base, provider_rate_limiter_t *rl,
                                     const char *url, struct curl_slist *headers,
                                     const char *body, long *out_http_code,
                                     provider_http_resp_t **out_response)
{
    int attempt = 0;
    int max_attempts = base->max_retries > 0 ? base->max_retries : PROVIDER_RL_MAX_RETRIES;

    while (attempt < max_attempts) {
        int ret = rl_check_rpm(rl);
        if (ret != 0) {
            SVC_LOG_WARN("C-L02: RL: RATE-LIMIT url=%s reason=rpm_limit_reached "
                         "attempt=%d/%d",
                         url, attempt + 1, max_attempts);
            struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
            nanosleep(&ts, NULL);
            continue;
        }

        *out_response = NULL;
        /* 两层重试职责正交：本层只按 HTTP 状态重试（429 限流 / 5xx 服务端错误），
         * 传输层瞬时故障（DNS / 连接 / TLS 握手 / 超时）交 provider_http_post 的
         * 退避重试处理（N-3）。此处传 0 会让"连不上"这类最常见故障完全不重试，
         * 故必须把 provider 的重试预算透传下去。 */
        ret = provider_http_post(url, headers, body, base->timeout_sec, base->max_retries,
                                 out_response, out_http_code);

        if (ret == AIRY_OK && *out_http_code == 200) {
            rl_reset_429(rl);
            return AIRY_OK;
        }

        if (*out_http_code == 429) {
            int retry_after = parse_retry_after(*out_response ? (*out_response)->data : NULL);
            rl_record_429(rl, retry_after);

            SVC_LOG_WARN("C-L02: RL: RATE-LIMIT url=%s http_code=429 attempt=%d/%d "
                         "retry_after=%ds",
                         url, attempt + 1, max_attempts, retry_after);

            if (*out_response) {
                provider_http_resp_free(*out_response);
                *out_response = NULL;
            }

            int wait_ms = rl_get_wait_ms(rl, attempt);
            if (wait_ms > 0) {
                struct timespec ts = {.tv_sec = wait_ms / 1000,
                                      .tv_nsec = (wait_ms % 1000) * 1000000LL};
                nanosleep(&ts, NULL);
            }
            attempt++;
            continue;
        }

        if (*out_http_code >= 500 && *out_http_code < 600 && attempt < max_attempts - 1) {
            SVC_LOG_WARN("C-L02: RL: SERVER-ERROR url=%s http_code=%ld attempt=%d/%d "
                         "retrying",
                         url, *out_http_code, attempt + 1, max_attempts);

            if (*out_response) {
                provider_http_resp_free(*out_response);
                *out_response = NULL;
            }

            int delay = PROVIDER_RL_BASE_DELAY_MS << attempt;
            if (delay > PROVIDER_RL_MAX_DELAY_MS)
                delay = PROVIDER_RL_MAX_DELAY_MS;
            struct timespec ts = {.tv_sec = delay / 1000, .tv_nsec = (delay % 1000) * 1000000LL};
            nanosleep(&ts, NULL);
            attempt++;
            continue;
        }

        break;
    }

    /* R-1：把 provider 返回的 HTTP 状态码映射为具体错误码，避免调用方把
     * 401/403（密钥无效）与 429（限流）统统当成"网络请求失败"上报。
     * http_code == 0 表示 curl 根本没连上 provider（DNS/连接/超时），
     * 此路径仍归 AIRY_ERR_IO。
     * N-3（0.1.16）：400/422 是 provider 明确拒绝请求体（JSON 结构错误、
     * 消息含无效 UTF-8 等），与"不可达"是两类失败——归入专用码
     * AIRY_ERR_LLM_BAD_REQUEST，既不被外层重试，也能给用户正确指引。 */
    switch (*out_http_code) {
    case 401:
    case 403:
        return AIRY_ERR_LLM_AUTH_FAIL;
    case 429:
        return AIRY_ERR_LLM_RATE_LIMIT;
    case 400:
    case 422:
        return AIRY_ERR_LLM_BAD_REQUEST;
    default:
        return AIRY_ERR_IO;
    }
}
