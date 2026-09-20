// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file http.c
 * @brief Provider 公共 HTTP 传输层（非流式 POST）。
 *
 * 域拆分自 provider.c（2026-08-27）：HTTP 请求执行 / 超时 / 响应体累积与
 * 释放；重试循环壳于 c8 收敛到 retry.c。SSE 流式传输见 sse.c。
 */

#include "airy_memory.h"
#include "error.h"
#include "rate_limit.h" /* provider_http_exec 的限流分支（provider_request_t.rl） */
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

/* 非流式单次尝试的上下文：写回调绑定到响应缓冲。 */
typedef struct {
    const char *url;
    struct curl_slist *headers;
    const char *body;
    double timeout_sec;
    provider_http_resp_t *resp;
} http_try_ctx_t;

static provider_attempt_t http_try(void *ud, int retry, double left_sec, CURLcode *out_res,
                                   long *out_http_code)
{
    http_try_ctx_t *ctx = (http_try_ctx_t *)ud;

    CURL *curl = curl_easy_init();
    if (!curl) {
        SVC_LOG_ERROR("C-L02: PROVIDER: HTTP-POST-FAIL url=%s errno=%d retry=%d "
                      "STACK: http_try curl_easy_init",
                      ctx->url, errno, retry);
        return PROVIDER_TRY_FATAL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, ctx->url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ctx->body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, ctx->headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx->resp);
    /* 首试给足全额预算；重试用整轮剩余墙钟，避免单次尝试越过总预算。 */
    provider_http_setup(curl, retry == 0 ? ctx->timeout_sec : left_sec);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out_http_code);
    curl_easy_cleanup(curl);

    *out_res = res;
    return res == CURLE_OK ? PROVIDER_TRY_DONE : PROVIDER_TRY_FAILED;
}

/* 重试前丢弃上一次尝试可能已写入的部分响应体。 */
static void http_try_reset(void *ud)
{
    provider_http_resp_t *resp = ((http_try_ctx_t *)ud)->resp;
    AIRY_FREE(resp->data);
    resp->data = NULL;
    resp->size = 0;
    resp->capacity = 0;
}

int provider_http_post(const char *url, struct curl_slist *headers, const char *body,
                       double timeout_sec, int max_retries, provider_http_resp_t **out_response,
                       long *out_http_code)
{
    if (!url || !body || !out_response || !out_http_code) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }

    provider_http_resp_t *resp =
        (provider_http_resp_t *)AIRY_CALLOC(1, sizeof(provider_http_resp_t));
    if (!resp)
        return AIRY_ERR_OUT_OF_MEMORY;

    http_try_ctx_t ctx = {
        .url = url,
        .headers = headers,
        .body = body,
        .timeout_sec = timeout_sec,
        .resp = resp,
    };
    provider_retry_cfg_t cfg = {
        .tag = "HTTP-POST",
        .url = url,
        .timeout_sec = timeout_sec,
        .max_retries = max_retries,
        .attempt = http_try,
        .gate = NULL,
        .reset = http_try_reset,
        .ud = &ctx,
    };

    CURLcode res = CURLE_OK;
    long http_code = 0;
    int rc = provider_retry_run(&cfg, &res, &http_code);
    if (rc != AIRY_OK || res != CURLE_OK) {
        provider_http_resp_free(resp);
        return rc != AIRY_OK ? rc : AIRY_ERR_IO;
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

/* 头链装配 SSoT（B16-S3.1）：鉴权样式 + Content-Type + 厂商静态附加头。适配层
 * 只声明 provider_header_spec_t，拼装、鉴权缓冲擦除与头链释放全部由本件兜住
 * （provider_http_exec 出口统一 free），故适配层不出现任何 curl_* 调用。 */
static struct curl_slist *provider_headers_build(const provider_base_ctx_t *base,
                                                 const provider_header_spec_t *spec)
{
    struct curl_slist *headers = NULL;

    if (spec && spec->auth != PROVIDER_AUTH_NONE) {
        if (base->api_key[0]) {
            char auth_header[1024];
            snprintf(auth_header, sizeof(auth_header),
                     spec->auth == PROVIDER_AUTH_X_API_KEY ? "x-api-key: %s"
                                                           : "Authorization: Bearer %s",
                     base->api_key);
            headers = curl_slist_append(headers, auth_header);
            explicit_bzero(auth_header, sizeof(auth_header));
        } else {
            /* R-1：未配置密钥时不拼空凭据——部分网关会因空 Bearer 返回难以定位
             * 的 400/401。无鉴权的本地端点用 PROVIDER_AUTH_NONE，不进本分支，
             * 因此不会产生本告警噪音。 */
            SVC_LOG_WARN("C-L02: PROVIDER: no API key configured, sending unauthenticated "
                         "request api_base=%s",
                         base->api_base);
        }
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");

    if (spec) {
        for (size_t i = 0; i < spec->extra_count; ++i)
            headers = curl_slist_append(headers, spec->extra[i]);
    }

    return headers;
}

int provider_http_exec(const provider_request_t *req, provider_http_resp_t **out_response,
                       long *out_http_code)
{
    int streaming = req && (req->on_event || req->on_chunk);
    if (!req || !req->base || !req->path || !req->body || !out_http_code ||
        (!streaming && !out_response)) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }

    char url[1024];
    snprintf(url, sizeof(url), "%s%s", req->base->api_base, req->path);

    struct curl_slist *headers = provider_headers_build(req->base, req->headers);

    int rc;
    if (req->on_event) {
        rc = provider_http_post_stream_sse(url, headers, req->body, req->base->timeout_sec,
                                           req->base->max_retries, req->on_event, req->user_data,
                                           out_http_code);
    } else if (req->on_chunk) {
        rc = provider_http_post_stream(url, headers, req->body, req->base->timeout_sec,
                                       req->base->max_retries, req->on_chunk, req->user_data,
                                       out_http_code);
    } else if (req->rl) {
        rc = provider_http_request_with_retry(req->base, req->rl, url, headers, req->body,
                                              out_http_code, out_response);
    } else {
        rc = provider_http_post(url, headers, req->body, req->base->timeout_sec,
                                req->base->max_retries, out_response, out_http_code);
    }

    if (headers)
        curl_slist_free_all(headers);
    return rc;
}
