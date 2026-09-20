// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file retry.c
 * @brief Provider 域退避重试机制唯一实现（B16-S3 c8）。
 *
 * 判断件（可重试性 / 指数退避 / 墙钟预算）与循环壳同处一件：非流式
 * （core/http.c 的 provider_http_post）与流式（core/sse.c 的
 * provider_stream_post）共用同一循环壳，各自只提供"单次尝试装配"与
 * "重试间复位"两个钩子，流式另行提供"已下发即禁止重试"闸门。
 * 适配层只填适配契约，禁止自持 curl/重试循环副本。
 */

#include "daemon_platform_ext.h"
#include "error.h"
#include "transport.h"
#include "svc_logger.h"

#include <errno.h>

int provider_retryable(CURLcode code)
{
    switch (code) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_SEND_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
        return 1;
    default:
        /* 请求本身非法（URL/协议）、调用方主动取消（WRITE_ERROR）、证书与
         * 证书链不受信任（PEER_FAILED_VERIFICATION 等）都不是瞬时故障，
         * 重试只会把调用方的预算空转掉。 */
        return 0;
    }
}

uint32_t provider_backoff_ms(int attempt)
{
    uint32_t backoff = PROVIDER_RETRY_BASE_MS;
    for (int i = 0; i < attempt && backoff < PROVIDER_RETRY_MAX_MS; i++)
        backoff <<= 1;
    if (backoff > PROVIDER_RETRY_MAX_MS)
        backoff = PROVIDER_RETRY_MAX_MS;

    uint32_t jitter = backoff * PROVIDER_RETRY_JITTER_PCT / 100U;
    if (jitter == 0)
        return backoff;
    return backoff - jitter + airy_random_uint32(0, jitter * 2U);
}

double provider_left_sec(double timeout_sec, uint64_t start_ms)
{
    double spent = (double)(airy_time_ms() - start_ms) / 1000.0;
    if (spent >= timeout_sec)
        return 0.0;
    return timeout_sec - spent;
}

int provider_retry_budget_ok(double timeout_sec, uint64_t start_ms, uint32_t delay_ms)
{
    double left = provider_left_sec(timeout_sec, start_ms);
    return left * 1000.0 >= (double)delay_ms + (double)PROVIDER_RETRY_MIN_LEFT_MS;
}

int provider_retry_run(const provider_retry_cfg_t *cfg, CURLcode *out_res, long *out_http_code)
{
    if (!cfg || !cfg->attempt || !cfg->url || !cfg->tag || !out_res || !out_http_code) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }

    int max_retries = cfg->max_retries > 0 ? cfg->max_retries : 0;
    uint64_t start_ms = airy_time_ms();
    CURLcode res = CURLE_OK;
    long http_code = 0;

    for (int retry = 0; retry <= max_retries; retry++) {
        double left = provider_left_sec(cfg->timeout_sec, start_ms);

        provider_attempt_t st = cfg->attempt(cfg->ud, retry, left, &res, &http_code);
        if (st == PROVIDER_TRY_FATAL)
            return AIRY_ERR_UNKNOWN;
        if (st == PROVIDER_TRY_DONE)
            break;

        if (retry >= max_retries || !provider_retryable(res) ||
            (cfg->gate && !cfg->gate(cfg->ud, res, http_code))) {
            SVC_LOG_WARN("C-L02: PROVIDER: %s-NORETRY url=%s errno=%d attempts=%d/%d "
                         "http_code=%ld diag=%s curl_error=%s retryable=%d",
                         cfg->tag, cfg->url, errno, retry + 1, max_retries + 1, http_code,
                         provider_http_diag(res), curl_easy_strerror(res),
                         provider_retryable(res));
            break;
        }

        uint32_t delay_ms = provider_backoff_ms(retry);
        if (!provider_retry_budget_ok(cfg->timeout_sec, start_ms, delay_ms)) {
            SVC_LOG_WARN("C-L02: PROVIDER: %s-STOP url=%s retry=%d/%d "
                         "reason=budget_spent left=%.1fs delay=%ums timeout=%.1fs",
                         cfg->tag, cfg->url, retry + 1, max_retries,
                         provider_left_sec(cfg->timeout_sec, start_ms), delay_ms,
                         cfg->timeout_sec);
            break;
        }

        SVC_LOG_WARN("C-L02: PROVIDER: %s-RETRY url=%s retry=%d/%d delay=%ums diag=%s "
                     "curl_error=%s",
                     cfg->tag, cfg->url, retry + 1, max_retries, delay_ms,
                     provider_http_diag(res), curl_easy_strerror(res));

        airy_sleep_ms(delay_ms);
        if (cfg->reset)
            cfg->reset(cfg->ud);
    }

    *out_res = res;
    *out_http_code = http_code;
    return AIRY_OK;
}
