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
#include "error.h"
#include "provider.h"
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

    CURL *curl = NULL;
    int retry = 0;
    int success = -1;
    CURLcode res;
    long http_code = 0;

    while (retry <= max_retries) {
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
        provider_http_setup(curl, timeout_sec);

        res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            success = 0;
            curl_easy_cleanup(curl);
            break;
        }

        SVC_LOG_WARN("C-L02: PROVIDER: HTTP-POST-FAIL url=%s errno=%d retry=%d/%d "
                     "diag=%s curl_error=%s",
                     url, errno, retry + 1, max_retries, provider_http_diag(res),
                     curl_easy_strerror(res));
        retry++;
        curl_easy_cleanup(curl);
        if (retry <= max_retries) {
            AIRY_FREE(resp->data);
            resp->data = NULL;
            resp->size = 0;
            resp->capacity = 0;
        }
    }

    if (success != 0) {
        provider_http_resp_free(resp);
        return AIRY_ERR_IO;
    }

    *out_response = resp;
    *out_http_code = http_code;
    return AIRY_OK;
}
