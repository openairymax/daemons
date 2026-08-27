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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            success = 0;
            curl_easy_cleanup(curl);
            break;
        }

        SVC_LOG_WARN("C-L02: PROVIDER: HTTP-POST-FAIL url=%s errno=%d retry=%d/%d "
                     "curl_error=%s",
                     url, errno, retry + 1, max_retries, curl_easy_strerror(res));
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
