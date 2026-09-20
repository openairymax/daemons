// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sse.c
 * @brief Provider 域唯一 SSE 分帧状态机（B16-S3 c4：三套收敛为一套）。
 *
 * 双模式：
 * - 行协议模式（provider_http_post_stream）：SSE data 行载荷 NUL 终结后交付
 *   provider_stream_chunk_cb_t，"[DONE]" 置正常终止。openai/deepseek/local
 *   走此模式。
 * - 具名事件模式（provider_http_post_stream_sse）：event:/data: 双行解析，
 *   事件名与载荷一并交付 provider_sse_event_cb_t，空行复位事件块。
 *   anthropic/google 走此模式。
 *
 * 两模式共享分帧（CR 剥离、缓冲扫描、重试复位）、错误体诊断（raw_buf）与
 * 重试循环；适配层只填回调，禁止自持 curl 循环副本。域拆分自 provider.c
 * （2026-08-27）；anthropic/google 私有 SSE 状态机于 c4 收敛至此；流式重试
 * 循环于 c8 收敛至 core/retry.c（本件只提供单次尝试与两个钩子）。
 */

#include "airy_llm_stream.h"
#include "airy_memory.h"
#include "error.h"
#include "transport.h"
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

/* ── 流式控制帧发射（帧格式权威：commons/include/airy_llm_stream.h，
 * openai/deepseek/local 共用，见 provider.h） ──
 * 帧各段须 NUL 结尾：llm_stream_callback 对 chunk 调 strlen()，非结尾数组
 * 会栈越界（ASan 2026-08-16 实测捕获）。 */
void provider_emit_tool_frame(llm_stream_callback_t cb, void *ud, const char *tc_json)
{
    if (!cb || !tc_json)
        return;
    char pre[3] = {(char)AIRY_LLM_STREAM_RS, AIRY_LLM_STREAM_TAG_TOOL, '\0'};
    char post[2] = {(char)AIRY_LLM_STREAM_RS, '\0'};
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
    char pre[3] = {(char)AIRY_LLM_STREAM_RS, AIRY_LLM_STREAM_TAG_REASON, '\0'};
    char post[2] = {(char)AIRY_LLM_STREAM_RS, '\0'};
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
    /* 双模式回调互斥：on_event 非空走具名事件模式，否则 on_chunk 行协议，
     * 由两个公开入口分别保证。 */
    provider_stream_chunk_cb_t on_chunk;
    provider_sse_event_cb_t on_event;
    void *chunk_user_data;
    char event_name[64]; /* 具名事件模式：当前事件块缓存，空行复位 */
    int cancelled;
    int done;
} sse_stream_ctx_t;

static void sse_ctx_init(sse_stream_ctx_t *sse, void *user_data)
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

    /* 具名事件模式：缓存 event: 字段；其余非 data 字段（id:/retry:/注释行）
     * 对两模式都无意义，落入下方前缀判断即被忽略。 */
    if (sse->on_event && len >= 6 && memcmp(line, "event:", 6) == 0) {
        const char *ev = line + 6;
        while (*ev == ' ' || *ev == '\t')
            ev++;
        size_t elen = len - (size_t)(ev - line);
        if (elen >= sizeof(sse->event_name))
            elen = sizeof(sse->event_name) - 1;
        __builtin_memcpy(sse->event_name, ev, elen);
        sse->event_name[elen] = '\0';
        return 0;
    }

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

        if (sse->on_chunk || sse->on_event) {
            /* 交付 NUL 终结副本：适配层回调（cJSON 解析 / strlen 语义）都
             * 要求结尾字符串，line_buf 内部切片不满足。 */
            char *tmp = (char *)AIRY_MALLOC(data_len + 1);
            if (tmp) {
                __builtin_memcpy(tmp, data_start, data_len);
                tmp[data_len] = '\0';
                int ret = sse->on_event ?
                              sse->on_event(sse->event_name[0] ? sse->event_name : NULL, tmp,
                                            data_len, sse->chunk_user_data) :
                              sse->on_chunk(tmp, sse->chunk_user_data);
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
        } else if (sse->on_event) {
            /* SSE 规范：空行终止一个事件块，具名事件缓存随之失效。 */
            sse->event_name[0] = '\0';
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

/* 单次流式尝试的上下文：写回调绑定到分帧状态机。 */
typedef struct {
    const char *url;
    struct curl_slist *headers;
    const char *body;
    double timeout_sec;
    sse_stream_ctx_t *sse;
} sse_try_ctx_t;

/* 单次尝试：装配 curl、执行、取回响应码（重试壳在 core/retry.c）。 */
static provider_attempt_t sse_try(void *ud, int retry, double left_sec, CURLcode *out_res,
                                  long *out_http_code)
{
    sse_try_ctx_t *ctx = (sse_try_ctx_t *)ud;

    CURL *curl = curl_easy_init();
    if (!curl) {
        SVC_LOG_ERROR("C-L02: PROVIDER: STREAM-FAIL url=%s errno=%d retry=%d "
                      "STACK: sse_try curl_easy_init",
                      ctx->url, errno, retry);
        return PROVIDER_TRY_FATAL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, ctx->url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ctx->body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, ctx->headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx->sse);
    /* 首试给足全额预算；重试用整轮剩余墙钟，避免单次尝试越过总预算。 */
    provider_http_setup(curl, retry == 0 ? ctx->timeout_sec : left_sec);

    CURLcode res = curl_easy_perform(curl);
    /* 失败路径也要取码：上游先回状态码再断流时，收尾要按状态码打印错误体
     * 并归类（与 http_try 只在成功时取码不同）。 */
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out_http_code);
    curl_easy_cleanup(curl);

    *out_res = res;
    return res == CURLE_OK ? PROVIDER_TRY_DONE : PROVIDER_TRY_FAILED;
}

/* 流式重试闸门：只有上游一个字节都没下发（http_code == 0 且 raw_len == 0）才
 * 允许重试。一旦写回调收到过数据，分片可能已经经回调交付用户，重试会造成
 * 重复输出。 */
static int sse_gate(void *ud, CURLcode res, long http_code)
{
    (void)res;
    sse_stream_ctx_t *sse = ((sse_try_ctx_t *)ud)->sse;
    return http_code == 0 && sse->raw_len == 0;
}

/* 重试前复位上一次尝试的分帧状态（闸门保证此时无字节下发，缓冲区保留复用）。 */
static void sse_reset(void *ud)
{
    sse_stream_ctx_t *sse = ((sse_try_ctx_t *)ud)->sse;
    sse->line_len = 0;
    sse->raw_len = 0;
    sse->done = 0;
    sse->cancelled = 0;
    sse->event_name[0] = '\0';
}

/* 两模式共用的流式入口：重试壳在 core/retry.c，本件只提供单次尝试与两个
 * 钩子。资源（line_buf/raw_buf）由公开入口持有，本函数任何路径不释放。 */
static int provider_stream_post(const char *url, struct curl_slist *headers, const char *body,
                                double timeout_sec, int max_retries, sse_stream_ctx_t *sse,
                                long *out_http_code)
{
    sse_try_ctx_t ctx = {
        .url = url,
        .headers = headers,
        .body = body,
        .timeout_sec = timeout_sec,
        .sse = sse,
    };
    provider_retry_cfg_t cfg = {
        .tag = "STREAM",
        .url = url,
        .timeout_sec = timeout_sec,
        .max_retries = max_retries,
        .attempt = sse_try,
        .gate = sse_gate,
        .reset = sse_reset,
        .ud = &ctx,
    };

    CURLcode res = CURLE_OK;
    long http_code = 0;
    int rc = provider_retry_run(&cfg, &res, &http_code);
    if (rc != AIRY_OK)
        return rc;

    *out_http_code = http_code;

    if (sse->line_len > 0) {
        sse_process_buffer(sse);
    }

    /* Error status: surface the upstream body (kept raw) so failures are
     * diagnosable from the daemon log instead of a bare http_code. */
    if (http_code >= 400 && sse->raw_buf && sse->raw_len > 0) {
        size_t n = sse->raw_len;
        if (n > 1024)
            n = 1024;
        SVC_LOG_ERROR("C-L02: PROVIDER: STREAM-HTTP-ERROR url=%s http_code=%ld body=%.*s",
                      url, http_code, (int)n, sse->raw_buf);
    }

    if (res != CURLE_OK) {
        SVC_LOG_WARN("C-L02: PROVIDER: STREAM-FAIL url=%s errno=%d diag=%s curl_error=%s", url,
                     errno, provider_http_diag(res), curl_easy_strerror(res));
        return AIRY_ERR_IO;
    }

    /* HTTP >= 400 is a failed completion, not a success with an empty body.
     * Falling through here previously surfaced provider rejections (e.g.
     * DeepSeek "tools[13].function.name" 400) as OK with a zero-token empty
     * stream, which clients rendered as "no reply / thinking only". */
    if (http_code == 400 || http_code == 422) {
        /* N-3（0.1.16）：provider 明确拒绝请求体（非法 JSON / 无效 UTF-8），
         * 归入专用码，避免与"网络不可达"（AIRY_ERR_IO）混为一谈。 */
        SVC_LOG_ERROR("C-L02: PROVIDER: STREAM-FAIL url=%s http_code=%ld "
                      "DIAGNOSIS=request_body_rejected",
                      url, http_code);
        return AIRY_ERR_LLM_BAD_REQUEST;
    }
    if (http_code >= 400) {
        SVC_LOG_ERROR("C-L02: PROVIDER: STREAM-FAIL url=%s http_code=%ld "
                      "DIAGNOSIS=upstream_http_error",
                      url, http_code);
        return AIRY_ERR_IO;
    }

    return AIRY_OK;
}

int provider_http_post_stream(const char *url, struct curl_slist *headers, const char *body,
                              double timeout_sec, int max_retries,
                              provider_stream_chunk_cb_t on_chunk, void *chunk_user_data,
                              long *out_http_code)
{
    if (!url || !body || !on_chunk || !out_http_code) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }
    if (max_retries < 0)
        max_retries = 0;

    sse_stream_ctx_t sse;
    sse_ctx_init(&sse, chunk_user_data);
    sse.on_chunk = on_chunk;
    if (!sse.line_buf) {
        sse_ctx_destroy(&sse);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    int ret = provider_stream_post(url, headers, body, timeout_sec, max_retries, &sse,
                                   out_http_code);
    sse_ctx_destroy(&sse);
    return ret;
}

int provider_http_post_stream_sse(const char *url, struct curl_slist *headers, const char *body,
                                  double timeout_sec, int max_retries,
                                  provider_sse_event_cb_t on_event, void *event_user_data,
                                  long *out_http_code)
{
    if (!url || !body || !on_event || !out_http_code) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }
    if (max_retries < 0)
        max_retries = 0;

    sse_stream_ctx_t sse;
    sse_ctx_init(&sse, event_user_data);
    sse.on_event = on_event;
    if (!sse.line_buf) {
        sse_ctx_destroy(&sse);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    int ret = provider_stream_post(url, headers, body, timeout_sec, max_retries, &sse,
                                   out_http_code);
    sse_ctx_destroy(&sse);
    return ret;
}
