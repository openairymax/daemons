// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file openai_rate_limit.c
 * @brief OpenAI 适配器令牌桶限流与重试。
 *
 * 域拆分自 openai.c（2026-08-27）：
 * - RPM/TPM 令牌桶窗口计数
 * - HTTP 429 检测与 Retry-After 解析
 * - 指数退避 + 抖动
 * - openai_http_request_with_retry（非流式 complete 路径专用）
 *
 * openai_rl_init / openai_rl_destroy / openai_http_request_with_retry 由
 * openai.c（生命周期与非流式 complete）跨文件调用，声明见 openai_internal.h；
 * 其余为文件内 static。
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "error.h"
#include "openai_internal.h"
#include "provider.h"
#include "svc_logger.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void openai_rl_init(openai_rate_limiter_t *rl)
{
    airy_mtx_init(&rl->lock);
    rl->rpm_window_start = time(NULL);
    rl->rpm_count = 0;
    rl->rpm_limit = OPENAI_DEFAULT_RPM;
    rl->tpm_count = 0;
    rl->tpm_window_start = time(NULL);
    rl->tpm_limit = OPENAI_DEFAULT_TPM;
    rl->last_429_time = 0;
    rl->retry_after_sec = 0;
    rl->consecutive_429s = 0;
}

void openai_rl_destroy(openai_rate_limiter_t *rl)
{
    airy_mtx_destroy(&rl->lock);
}

static int openai_rl_check_rpm(openai_rate_limiter_t *rl)
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

static int __attribute__((unused)) openai_rl_check_tpm(openai_rate_limiter_t *rl, int tokens)
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

static void openai_rl_record_429(openai_rate_limiter_t *rl, int retry_after)
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

static int openai_rl_get_wait_ms(openai_rate_limiter_t *rl, int attempt)
{
    airy_mtx_lock(&rl->lock);

    int wait_ms;

    if (rl->retry_after_sec > 0) {
        wait_ms = rl->retry_after_sec * 1000;
        rl->retry_after_sec = 0;
    } else {
        int base_delay = OPENAI_BASE_DELAY_MS << attempt;
        if (base_delay > OPENAI_MAX_DELAY_MS)
            base_delay = OPENAI_MAX_DELAY_MS;

        double jitter =
            ((double)airy_random_uint32(0, 99) / 100.0) * base_delay * OPENAI_JITTER_FACTOR;
        wait_ms = (int)((double)base_delay + jitter);
    }

    airy_mtx_unlock(&rl->lock);
    return wait_ms;
}

static void openai_rl_reset_429(openai_rate_limiter_t *rl)
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

int openai_http_request_with_retry(openai_ctx_t *ctx, const char *url,
                                   struct curl_slist *headers, const char *body,
                                   long *out_http_code, provider_http_resp_t **out_response)
{
    int attempt = 0;
    int max_attempts = ctx->base.max_retries > 0 ? ctx->base.max_retries : OPENAI_MAX_RETRIES;

    while (attempt < max_attempts) {
        int ret = openai_rl_check_rpm(&ctx->rl);
        if (ret != 0) {
            SVC_LOG_WARN("C-L02: OPENAI: RATE-LIMIT url=%s reason=rpm_limit_reached "
                         "attempt=%d/%d",
                         url, attempt + 1, max_attempts);
            struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
            nanosleep(&ts, NULL);
            continue;
        }

        *out_response = NULL;
        ret = provider_http_post(url, headers, body, ctx->base.timeout_sec, 0, out_response,
                                 out_http_code);

        if (ret == AIRY_OK && *out_http_code == 200) {
            openai_rl_reset_429(&ctx->rl);
            return AIRY_OK;
        }

        if (*out_http_code == 429) {
            int retry_after = parse_retry_after(*out_response ? (*out_response)->data : NULL);
            openai_rl_record_429(&ctx->rl, retry_after);

            SVC_LOG_WARN("C-L02: OPENAI: RATE-LIMIT url=%s http_code=429 attempt=%d/%d "
                         "retry_after=%ds",
                         url, attempt + 1, max_attempts, retry_after);

            if (*out_response) {
                provider_http_resp_free(*out_response);
                *out_response = NULL;
            }

            int wait_ms = openai_rl_get_wait_ms(&ctx->rl, attempt);
            if (wait_ms > 0) {
                struct timespec ts = {.tv_sec = wait_ms / 1000,
                                      .tv_nsec = (wait_ms % 1000) * 1000000LL};
                nanosleep(&ts, NULL);
            }
            attempt++;
            continue;
        }

        if (*out_http_code >= 500 && *out_http_code < 600 && attempt < max_attempts - 1) {
            SVC_LOG_WARN("C-L02: OPENAI: SERVER-ERROR url=%s http_code=%ld attempt=%d/%d "
                         "retrying",
                         url, *out_http_code, attempt + 1, max_attempts);

            if (*out_response) {
                provider_http_resp_free(*out_response);
                *out_response = NULL;
            }

            int delay = OPENAI_BASE_DELAY_MS << attempt;
            if (delay > OPENAI_MAX_DELAY_MS)
                delay = OPENAI_MAX_DELAY_MS;
            struct timespec ts = {.tv_sec = delay / 1000, .tv_nsec = (delay % 1000) * 1000000LL};
            nanosleep(&ts, NULL);
            attempt++;
            continue;
        }

        break;
    }

    return AIRY_ERR_IO;
}
