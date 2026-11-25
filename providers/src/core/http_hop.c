/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file http_hop.c
 * @brief 单跳、不跟随重定向的 GET 传输（N-3）。
 *
 *        provider_http_get() 复用 provider_http_setup()，而后者无条件开启
 *        CURLOPT_FOLLOWLOCATION：一旦跟随，重定向目标就不会再经过调用方的
 *        URL 策略（SSRF 主机规则）。本件提供单跳 GET——3xx 只回报 Location，
 *        由调用方复核后自行发下一跳，逐跳口径与传输策略因此可以分离。
 *
 *        响应体按调用方声明的 max_body 截断累积，避免不可信页面把内存吃穿；
 *        失败阶段直接写回 out->why，不另建线程局域诊断槽。
 */

#include "airy_prov/provider_http.h"

#include "airy_memory.h"
#include "error.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* 写入端上下文：max_body 为 0 时不限；超出部分静默丢弃但保持传输继续，
 * 让调用方仍能拿到状态码与截断后的正文。 */
typedef struct {
    provider_http_resp_t *resp;
    size_t max_body;
} hop_sink_t;

static size_t hop_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t n = size * nmemb;
    hop_sink_t *sink = (hop_sink_t *)userp;
    provider_http_resp_t *resp = sink->resp;

    if (sink->max_body && resp->size >= sink->max_body)
        return n;
    if (sink->max_body && n > sink->max_body - resp->size)
        n = sink->max_body - resp->size;

    size_t need = resp->size + n + 1;
    if (need > resp->capacity) {
        size_t cap = resp->capacity ? resp->capacity : 4096;
        while (cap < need)
            cap *= 2;
        char *p = (char *)AIRY_REALLOC(resp->data, cap);
        if (!p)
            return 0;
        resp->data = p;
        resp->capacity = cap;
    }

    AIRY_MEMCPY(resp->data + resp->size, contents, n);
    resp->size += n;
    resp->data[resp->size] = '\0';
    return size * nmemb;
}

static char hop_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* 头名按 ASCII 大小写不敏感比较，不依赖 strncasecmp 的三端声明。 */
static int hop_is_location(const char *buf, size_t len)
{
    static const char name[] = "location:";

    if (len <= sizeof(name) - 1)
        return 0;
    for (size_t i = 0; i < sizeof(name) - 1; i++) {
        if (hop_lower(buf[i]) != name[i])
            return 0;
    }
    return 1;
}

/* 只抓首个 Location：去前导空白与行尾 CR/LF。 */
static size_t hop_header_cb(char *buf, size_t size, size_t nitems, void *userp)
{
    static const char name[] = "location:";
    char **out = (char **)userp;
    size_t len = size * nitems;

    if (!hop_is_location(buf, len) || *out)
        return len;

    char *p = buf + sizeof(name) - 1;
    char *end = buf + len;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    while (end > p && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t'))
        end--;
    if (end == p)
        return len;

    size_t n = (size_t)(end - p);
    char *loc = (char *)AIRY_MALLOC(n + 1);
    if (!loc)
        return len;
    AIRY_MEMCPY(loc, p, n);
    loc[n] = '\0';
    *out = loc;
    return len;
}

int prov_hop_get(const prov_hop_req_t *req, prov_hop_resp_t *out)
{
    if (!req || !out || !req->url || !req->url[0]) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }
    AIRY_MEMSET(out, 0, sizeof(*out));

    provider_http_resp_t *resp = (provider_http_resp_t *)AIRY_CALLOC(1, sizeof(*resp));
    if (!resp)
        return AIRY_ERR_OUT_OF_MEMORY;

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(out->why, sizeof(out->why), "curl init failed");
        AIRY_FREE(resp);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    hop_sink_t sink = {resp, req->max_body};
    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req->headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, hop_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, hop_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out->location);
    provider_http_setup(curl, req->timeout_sec, 0.0);
    /* setup 按整秒取整，亚秒预算会被折成 0（libcurl 视为无超时）；按毫秒补齐。 */
    if (req->timeout_sec > 0.0) {
        long ms = (long)(req->timeout_sec * 1000.0);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, ms > 0 ? ms : 1L);
    }
    /* provider_http_setup 无条件打开 FOLLOWLOCATION；逐跳复核要求这里关掉，
     * 否则重定向目标会绕过调用方的 URL 策略。 */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        snprintf(out->why, sizeof(out->why), "%s", provider_http_diag(res));
        AIRY_FREE(out->location);
        out->location = NULL;
        provider_http_resp_free(resp);
        return AIRY_ERR_IO;
    }

    /* 只有重定向才向调用方交付 Location；其余响应的同名头是噪声。 */
    if (code < 300 || code >= 400) {
        AIRY_FREE(out->location);
        out->location = NULL;
    }

    out->response = resp;
    out->http_code = code;
    return AIRY_OK;
}
