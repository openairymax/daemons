// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file provider_http.c
 * @brief Provider 公共 HTTP 传输层（非流式 POST）。
 *
 * 域拆分自 provider.c（2026-08-27）：HTTP 请求执行 / 重试 / 超时 /
 * 响应体累积与释放。SSE 流式传输见 provider_stream.c。
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "error.h"
#include "transport.h"
#include "svc_logger.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void provider_http_resp_free(provider_http_resp_t *resp)
{
    if (resp) {
        AIRY_FREE(resp->data);
        AIRY_FREE(resp);
    }
}

static size_t http_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    provider_http_resp_t *mem = (provider_http_resp_t *)userp;

    size_t new_size = mem->size + realsize + 1;
    if (new_size > mem->capacity) {
        size_t new_cap = mem->capacity * 2;
        if (new_cap < new_size)
            new_cap = new_size;

        char *ptr = (char *)AIRY_REALLOC(mem->data, new_cap);
        if (!ptr)
            return 0;

        mem->data = ptr;
        mem->capacity = new_cap;
    }

    __builtin_memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

/* 连接阶段默认超时（秒）。不设连接超时的后果是"网络总接不通"：DNS 无应答
 * 或 TCP SYN 被丢弃时，libcurl 会一直等到总超时（默认 70s）才放弃，用户看到
 * 的是一次请求长时间无响应。可按环境变量覆盖。 */
#define PROVIDER_CONNECT_TIMEOUT_SEC 10L

static const char *env_first_set(const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const char *v = getenv(names[i]);
        if (v && v[0])
            return v;
    }
    return NULL;
}

void provider_http_setup(CURL *curl, double timeout_sec)
{
    if (!curl)
        return;

    long total_sec = (long)timeout_sec;
    long connect_sec = PROVIDER_CONNECT_TIMEOUT_SEC;

    const char *env_connect = getenv("AIRY_HTTP_CONNECT_TIMEOUT");
    if (env_connect && env_connect[0]) {
        long v = strtol(env_connect, NULL, 10);
        if (v > 0)
            connect_sec = v;
    }
    if (total_sec > 0 && connect_sec >= total_sec)
        connect_sec = total_sec > 1 ? total_sec - 1 : 1;

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, total_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_sec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    static const char *const proxy_names[] = {"AIRY_HTTP_PROXY", "HTTPS_PROXY", "https_proxy",
                                              "ALL_PROXY",       "all_proxy",   "HTTP_PROXY",
                                              "http_proxy"};
    const char *proxy = env_first_set(proxy_names, sizeof(proxy_names) / sizeof(proxy_names[0]));
    if (proxy)
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy);

    static const char *const ca_names[] = {"AIRY_HTTP_CAINFO", "CURL_CA_BUNDLE", "SSL_CERT_FILE"};
    const char *ca = env_first_set(ca_names, sizeof(ca_names) / sizeof(ca_names[0]));
    if (ca)
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
}

const char *provider_http_diag(CURLcode code)
{
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return "DNS_FAIL";
    case CURLE_COULDNT_CONNECT:
        return "CONNECT_FAIL";
    case CURLE_OPERATION_TIMEDOUT:
        return "TIMEOUT";
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
        return "TLS_FAIL";
    case CURLE_PROXY:
        return "PROXY_FAIL";
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
        return "NET_IO_FAIL";
    default:
        return "OTHER";
    }
}

/* 出网重试策略唯一实现（SSoT）。与 provider_http_setup / provider_http_diag 同属
 * 传输层策略：非流式与流式（provider_stream / google / anthropic）共用，禁止各自
 * 复制"哪些错误值得重试"与"退避多久"的判断。 */

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

int provider_http_post(const char *url, struct curl_slist *headers, const char *body,
                       double timeout_sec, int max_retries, provider_http_resp_t **out_response,
                       long *out_http_code)
{
    if (!url || !body || !out_response || !out_http_code) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }
    if (max_retries < 0)
        max_retries = 0;

    provider_http_resp_t *resp =
        (provider_http_resp_t *)AIRY_CALLOC(1, sizeof(provider_http_resp_t));
    if (!resp)
        return AIRY_ERR_OUT_OF_MEMORY;

    CURL *curl = NULL;
    CURLcode res = CURLE_OK;
    long http_code = 0;
    int success = -1;
    uint64_t start_ms = airy_time_ms();

    for (int retry = 0; retry <= max_retries; retry++) {
        double left = provider_left_sec(timeout_sec, start_ms);

        curl = curl_easy_init();
        if (!curl) {
            SVC_LOG_ERROR("C-L02: PROVIDER: HTTP-POST-FAIL url=%s errno=%d retry=%d/%d "
                          "STACK: provider_http_post curl_easy_init",
                          url, errno, retry, max_retries);
            provider_http_resp_free(resp);
            return AIRY_ERR_UNKNOWN;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
        provider_http_setup(curl, retry == 0 ? timeout_sec : left);

        res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            success = 0;
            curl_easy_cleanup(curl);
            break;
        }
        curl_easy_cleanup(curl);

        if (retry >= max_retries || !provider_retryable(res)) {
            SVC_LOG_WARN("C-L02: PROVIDER: HTTP-POST-FAIL url=%s errno=%d attempts=%d/%d "
                         "diag=%s curl_error=%s retryable=%d",
                         url, errno, retry + 1, max_retries + 1, provider_http_diag(res),
                         curl_easy_strerror(res), provider_retryable(res));
            break;
        }

        uint32_t delay_ms = provider_backoff_ms(retry);
        if (!provider_retry_budget_ok(timeout_sec, start_ms, delay_ms)) {
            SVC_LOG_WARN("C-L02: PROVIDER: HTTP-POST-STOP url=%s retry=%d/%d "
                         "reason=budget_spent left=%.1fs delay=%ums timeout=%.1fs",
                         url, retry + 1, max_retries, provider_left_sec(timeout_sec, start_ms),
                         delay_ms, timeout_sec);
            break;
        }
        SVC_LOG_WARN("C-L02: PROVIDER: HTTP-POST-RETRY url=%s retry=%d/%d delay=%ums "
                     "diag=%s curl_error=%s",
                     url, retry + 1, max_retries, delay_ms, provider_http_diag(res),
                     curl_easy_strerror(res));
        airy_sleep_ms(delay_ms);
        AIRY_FREE(resp->data);
        resp->data = NULL;
        resp->size = 0;
        resp->capacity = 0;
    }

    if (success != 0) {
        provider_http_resp_free(resp);
        return AIRY_ERR_IO;
    }

    *out_response = resp;
    *out_http_code = http_code;
    return AIRY_OK;
}

/* R-1 / N-3：provider HTTP 状态码 → airy 错误码归一（流式与非流式共用 SSoT，
 * 收敛 openai_stream 内联 switch 与 rate_limit 非流式 switch 的同构副本）。
 * 401/403（密钥无效）与 429（限流）此前被统称"网络请求失败"，用户无从排障；
 * 400/422 是 provider 明确拒绝请求体（N-3，0.1.16），归专用码——不被外层
 * 重试，也给用户正确指引。http_code == 0 表示请求未达上游，返回 fallback。 */
int provider_http_err_map(long http_code, int fallback)
{
    switch (http_code) {
    case 401:
    case 403:
        return AIRY_ERR_LLM_AUTH_FAIL;
    case 429:
        return AIRY_ERR_LLM_RATE_LIMIT;
    case 400:
    case 422:
        return AIRY_ERR_LLM_BAD_REQUEST;
    default:
        return fallback;
    }
}

/* 状态码 → 日志诊断串（与 err_map 配套，收敛各适配器 DIAGNOSIS= 分支）。 */
const char *provider_http_err_diag(long http_code)
{
    switch (http_code) {
    case 401:
    case 403:
        return "auth_failed";
    case 429:
        return "rate_limit_exhausted";
    case 400:
    case 422:
        return "request_body_rejected";
    case 0:
        return "transport_error";
    default:
        return "http_error";
    }
}

struct curl_slist *provider_openai_headers(const provider_base_ctx_t *base, const char *path,
                                           char *url_out, size_t url_cap)
{
    snprintf(url_out, url_cap, "%s%s", base->api_base, path);

    struct curl_slist *headers = NULL;
    if (base->api_key[0]) {
        char auth_header[1024];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", base->api_key);
        headers = curl_slist_append(headers, auth_header);
        explicit_bzero(auth_header, sizeof(auth_header));
    } else {
        /* R-1：未配置密钥时不再拼出 "Authorization: Bearer "（空凭据），
         * 部分网关会因此返回难以定位的 400/401；直接省略该头，本地
         * OpenAI 兼容服务（无鉴权）也能正常工作。 */
        SVC_LOG_WARN("C-L02: PROVIDER: no API key configured, sending unauthenticated request "
                     "url=%s",
                     url_out);
    }
    return curl_slist_append(headers, "Content-Type: application/json");
}
