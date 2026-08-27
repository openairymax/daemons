// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file provider_stream.c
 * @brief Provider 公共 SSE 流式传输与流式辅助。
 *
 * 域拆分自 provider.c（2026-08-27）：
 * - SSE 行解析与累积（sse_*）
 * - provider_http_post_stream（流式 HTTP POST）
 * - 流式控制帧发射（provider_emit_tool_frame / provider_emit_reasoning_frame）
 * - 增长缓冲 provider_buf_append
 */

#include "airy_memory.h"
#include "error.h"
#include "provider.h"
#include "svc_logger.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

char *provider_buf_append(char *buf, size_t *cap, size_t *len, const char *text)
{
    if (!text)
        return buf;
    size_t tlen = strlen(text);
    if (tlen == 0)
        return buf;

    size_t needed = *len + tlen + 1;
    if (needed > *cap) {
        size_t new_cap = *cap > 0 ? *cap : 4096;
        while (new_cap < needed)
            new_cap *= 2;
        char *ptr = (char *)AIRY_REALLOC(buf, new_cap);
        if (!ptr)
            return NULL;
        buf = ptr;
        *cap = new_cap;
    }
    __builtin_memcpy(buf + *len, text, tlen);
    *len += tlen;
    buf[*len] = '\0';
    return buf;
}

/* ── 流式控制帧发射（SSoT：openai/deepseek/local 共用，见 provider.h） ──
 * 帧各段须 NUL 结尾：llm_stream_callback 对 chunk 调 strlen()，非结尾数组
 * 会栈越界（ASan 2026-08-16 实测捕获）。 */
#define LLM_STREAM_FRAME_RS 0x1e
#define LLM_STREAM_FRAME_TAG 'T'
#define LLM_STREAM_FRAME_REASON_TAG 'R'

void provider_emit_tool_frame(llm_stream_callback_t cb, void *ud, const char *tc_json)
{
    if (!cb || !tc_json)
        return;
    char pre[3] = {(char)LLM_STREAM_FRAME_RS, LLM_STREAM_FRAME_TAG, '\0'};
    char post[2] = {(char)LLM_STREAM_FRAME_RS, '\0'};
    cb(pre, ud);
    cb(tc_json, ud);
    cb(post, ud);
}

/* Reasoning frame: RS 'R' <reasoning_content> RS。DeepSeek thinking 模式要求
 * assistant 轮的 reasoning_content 在工具续接轮回传（否则上游 400），流式
 * 路径必须透出，否则工具循环断裂。 */
void provider_emit_reasoning_frame(llm_stream_callback_t cb, void *ud, const char *reasoning)
{
    if (!cb || !reasoning || !reasoning[0])
        return;
    char pre[3] = {(char)LLM_STREAM_FRAME_RS, LLM_STREAM_FRAME_REASON_TAG, '\0'};
    char post[2] = {(char)LLM_STREAM_FRAME_RS, '\0'};
    cb(pre, ud);
    cb(reasoning, ud);
    cb(post, ud);
}

typedef struct {
    char *line_buf;
    size_t line_cap;
    size_t line_len;
    /* Raw response body (all bytes, SSE or not): kept so an error status
     * (e.g. upstream 400) can surface its JSON error body in the daemon log
     * instead of being silently dropped by the SSE line parser (2026-08-16). */
    char *raw_buf;
    size_t raw_cap;
    size_t raw_len;
    provider_stream_chunk_cb_t on_chunk;
    void *chunk_user_data;
    int cancelled;
    int done;
} sse_stream_ctx_t;

static void sse_ctx_init(sse_stream_ctx_t *sse, provider_stream_chunk_cb_t cb, void *user_data)
{
    __builtin_memset(sse, 0, sizeof(*sse));
    sse->line_cap = 4096;
    sse->line_buf = (char *)AIRY_MALLOC(sse->line_cap);
    if (!sse->line_buf) {
        SVC_LOG_ERROR("C-L02: PROVIDER: SSE-INIT-FAIL reason=oom cap=%zu "
                      "STACK: sse_ctx_init",
                      sse->line_cap);
    }
    sse->raw_cap = 4096;
    sse->raw_buf = (char *)AIRY_MALLOC(sse->raw_cap);
    if (!sse->raw_buf) {
        SVC_LOG_ERROR("C-L02: PROVIDER: SSE-INIT-FAIL reason=oom_raw_cap=%zu "
                      "STACK: sse_ctx_init",
                      sse->raw_cap);
    }
    sse->on_chunk = cb;
    sse->chunk_user_data = user_data;
}

static void sse_ctx_destroy(sse_stream_ctx_t *sse)
{
    if (sse) {
        AIRY_FREE(sse->line_buf);
        sse->line_buf = NULL;
        AIRY_FREE(sse->raw_buf);
        sse->raw_buf = NULL;
    }
}

/* Accumulate every received byte so an error body survives SSE parsing. */
static void sse_raw_append(sse_stream_ctx_t *sse, const char *data, size_t len)
{
    if (!sse->raw_buf || len == 0)
        return;
    size_t need = sse->raw_len + len + 1;
    if (need > sse->raw_cap) {
        size_t cap = sse->raw_cap * 2;
        while (cap < need)
            cap *= 2;
        char *grown = (char *)AIRY_REALLOC(sse->raw_buf, cap);
        if (!grown)
            return;
        sse->raw_buf = grown;
        sse->raw_cap = cap;
    }
    __builtin_memcpy(sse->raw_buf + sse->raw_len, data, len);
    sse->raw_len += len;
    sse->raw_buf[sse->raw_len] = '\0';
}

static int sse_feed_line(sse_stream_ctx_t *sse, const char *line, size_t len)
{
    if (!line || len == 0)
        return 0;

    if (len >= 5 && memcmp(line, "data:", 5) == 0) {
        const char *data_start = line + 5;
        while (*data_start == ' ' || *data_start == '\t')
            data_start++;
        size_t data_len = len - (size_t)(data_start - line);

        if (data_len >= 6 && memcmp(data_start, "[DONE]", 6) == 0) {
            /* Normal stream end: [DONE] is the standard SSE-protocol
             * terminator, not an error. Only set the done flag; cancelled is
             * left for on_chunk error cancellation — otherwise the curl write
             * callback returning 0 due to cancelled would falsely report
             * CURLE_WRITE_ERROR and treat a normal completion as STREAM-FAIL
             * (historical defect exposed by real streaming callers). */
            sse->done = 1;
            return 0;
        }

        if (sse->on_chunk) {
            char *tmp = (char *)AIRY_MALLOC(data_len + 1);
            if (tmp) {
                __builtin_memcpy(tmp, data_start, data_len);
                tmp[data_len] = '\0';
                int ret = sse->on_chunk(tmp, sse->chunk_user_data);
                AIRY_FREE(tmp);
                if (ret != 0) {
                    sse->cancelled = 1;
                    return ret;
                }
            }
        }
    }

    return 0;
}

static void sse_process_buffer(sse_stream_ctx_t *sse)
{
    if (sse->line_len == 0)
        return;

    char *p = sse->line_buf;
    char *end = p + sse->line_len;

    while (p < end) {
        char *nl = (char *)memchr(p, '\n', (size_t)(end - p));
        if (!nl)
            break;

        size_t line_len = (size_t)(nl - p);
        if (line_len > 0 && *(nl - 1) == '\r')
            line_len--;

        if (line_len > 0) {
            int r = sse_feed_line(sse, p, line_len);
            if (r != 0 || sse->cancelled)
                return;
        }

        p = nl + 1;
    }

    if (p < end) {
        size_t remaining = (size_t)(end - p);
        __builtin_memmove(sse->line_buf, p, remaining);
        sse->line_len = remaining;
    } else {
        sse->line_len = 0;
    }
}

static size_t sse_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    sse_stream_ctx_t *sse = (sse_stream_ctx_t *)userp;

    if (sse->cancelled)
        return 0;

    size_t needed = sse->line_len + realsize + 1;
    if (needed > sse->line_cap) {
        size_t new_cap = sse->line_cap * 2;
        while (new_cap < needed)
            new_cap *= 2;
        char *ptr = (char *)AIRY_REALLOC(sse->line_buf, new_cap);
        if (!ptr)
            return 0;
        sse->line_buf = ptr;
        sse->line_cap = new_cap;
    }

    __builtin_memcpy(sse->line_buf + sse->line_len, contents, realsize);
    sse->line_len += realsize;
    sse->line_buf[sse->line_len] = '\0';

    sse_raw_append(sse, (const char *)contents, realsize);

    sse_process_buffer(sse);

    if (sse->cancelled)
        return 0;
    return realsize;
}

int provider_http_post_stream(const char *url, struct curl_slist *headers, const char *body,
                              double timeout_sec, provider_stream_chunk_cb_t on_chunk,
                              void *chunk_user_data, long *out_http_code)
{
    if (!url || !body || !on_chunk || !out_http_code) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }

    sse_stream_ctx_t sse;
    sse_ctx_init(&sse, on_chunk, chunk_user_data);
    if (!sse.line_buf) {
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        sse_ctx_destroy(&sse);
        SVC_LOG_ERROR("C-L02: PROVIDER: STREAM-FAIL url=%s errno=%d "
                      "STACK: provider_http_post_stream curl_easy_init",
                      url, errno);
        return AIRY_ERR_UNKNOWN;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sse);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    *out_http_code = http_code;
    curl_easy_cleanup(curl);

    if (sse.line_len > 0) {
        sse_process_buffer(&sse);
    }

    /* Error status: surface the upstream body (kept raw) so failures are
     * diagnosable from the daemon log instead of a bare http_code. */
    if (http_code >= 400 && sse.raw_buf && sse.raw_len > 0) {
        size_t n = sse.raw_len;
        if (n > 1024)
            n = 1024;
        SVC_LOG_ERROR("C-L02: PROVIDER: STREAM-HTTP-ERROR url=%s http_code=%ld body=%.*s",
                      url, http_code, (int)n, sse.raw_buf);
    }

    sse_ctx_destroy(&sse);

    if (res != CURLE_OK) {
        SVC_LOG_WARN("C-L02: PROVIDER: STREAM-FAIL url=%s errno=%d curl_error=%s", url, errno,
                     curl_easy_strerror(res));
        return AIRY_ERR_IO;
    }

    /* HTTP >= 400 is a failed completion, not a success with an empty body.
     * Falling through here previously surfaced provider rejections (e.g.
     * DeepSeek "tools[13].function.name" 400) as OK with a zero-token empty
     * stream, which clients rendered as "no reply / thinking only". */
    if (http_code >= 400) {
        SVC_LOG_ERROR("C-L02: PROVIDER: STREAM-FAIL url=%s http_code=%ld "
                      "DIAGNOSIS=upstream_http_error",
                      url, http_code);
        return AIRY_ERR_IO;
    }

    return AIRY_OK;
}
