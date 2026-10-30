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
#include "secrets.h" /* explicit_bzero SSoT shim：macOS/Windows 无系统声明 */
#include "provider_http.h"
#include "provider_retry.h"
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

/* 连接阶段与读空闲的缺省值见 provider_transport.h。连接超时不设的后果是
 * "网络总接不通"：DNS 无应答或 TCP SYN 被丢弃时 libcurl 会一直等到总预算
 * 耗尽；读空闲不设的后果是"连接活着但上游不再产出"被当作成功回复——这是
 * 比超时更难发现的一类错误。两者都可按环境变量覆盖。 */
static const char *env_first_set(const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const char *v = getenv(names[i]);
        if (v && v[0])
            return v;
    }
    return NULL;
}

static long env_long(const char *name, long def)
{
    const char *v = getenv(name);
    if (!v || !v[0])
        return def;
    long parsed = strtol(v, NULL, 10);
    return parsed > 0 ? parsed : def;
}

void provider_http_setup(CURL *curl, double timeout_sec, double read_idle_sec)
{
    if (!curl)
        return;

    long total_sec = (long)timeout_sec;
    long connect_sec = env_long("AIRY_HTTP_CONNECT_TIMEOUT", (long)PROVIDER_CONNECT_TIMEOUT_SEC);
    double idle = read_idle_sec > 0.0 ? read_idle_sec : PROVIDER_READ_IDLE_SEC;
    long idle_sec = env_long("AIRY_HTTP_READ_IDLE", (long)idle);

    if (total_sec > 0 && connect_sec >= total_sec)
        connect_sec = total_sec > 1 ? total_sec - 1 : 1;
    /* 读空闲须严格早于总预算，否则停滞永远由总预算先判定，STALL 计数失效。 */
    if (total_sec > 0 && idle_sec >= total_sec)
        idle_sec = total_sec > 1 ? total_sec - 1 : 1;

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, total_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_sec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, idle_sec);
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

/* 超时三拆的传输侧判据。连接从未建立（CONNECT_TIME 为 0）说明请求未送达，
 * 可安全重试；总耗时已贴近整轮预算说明是预算不足而非上游停摆；其余情形为
 * 连接已建立但上游停止产出。用 double 版 CURLINFO 而非 _TIME_T 版，兼容旧
 * libcurl 与三端构建。 */
prov_tmo_t provider_tmo_class(CURL *curl, double budget_sec)
{
    if (!curl)
        return PROV_TMO_KIND_N;

    double total = 0.0;
    double connect = 0.0;
    if (curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total) != CURLE_OK)
        total = 0.0;
    if (curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect) != CURLE_OK)
        connect = 0.0;

    if (connect <= 0.0)
        return PROV_TMO_CONNECT;
    if (budget_sec > 0.0 && total >= budget_sec - 1.0)
        return PROV_TMO_TOTAL;
    return PROV_TMO_STALL;
}

void prov_http_done(CURL *curl, double budget_sec, CURLcode res, int *out_tmo)
{
    if (res == CURLE_OPERATION_TIMEDOUT) {
        prov_tmo_t kind = provider_tmo_class(curl, budget_sec);
        *out_tmo = (int)kind;
        if (kind != PROV_TMO_KIND_N)
            prov_stat_hit(kind);
    }
    curl_easy_cleanup(curl);
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
                                   long *out_http_code, int *out_tmo)
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
    double budget = retry == 0 ? ctx->timeout_sec : left_sec;
    provider_http_setup(curl, budget, PROVIDER_READ_IDLE_SEC);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out_http_code);
    prov_http_done(curl, budget, res, out_tmo);

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
                       double timeout_sec, int max_retries, provider_retry_class_t retry_class,
                       int idempotent, provider_http_resp_t **out_response, long *out_http_code)
{
    if (!url || !body || !out_response || !out_http_code) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }

    provider_http_resp_t *resp = (provider_http_resp_t *)AIRY_CALLOC(1, sizeof(*resp));
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
        .retry_class = retry_class,
        .idempotent = idempotent,
        .attempt = http_try,
        .gate = NULL,
        .reset = http_try_reset,
        .ud = &ctx,
    };

    CURLcode res = CURLE_OK;
    long http_code = 0;
    int tmo = (int)PROV_TMO_KIND_N;
    int rc = provider_retry_run(&cfg, &res, &http_code, &tmo);
    if (rc != AIRY_OK || res != CURLE_OK) {
        provider_http_resp_free(resp);
        if (rc != AIRY_OK)
            return rc;
        if (res != CURLE_OPERATION_TIMEDOUT)
            return AIRY_ERR_IO;
        /* 停摆可被上层按幂等性重试；总预算耗尽不可重试；连接未建立按 I/O 处理。 */
        if (tmo == (int)PROV_TMO_STALL)
            return AIRY_ERR_LLM_STREAM_STALL;
        if (tmo == (int)PROV_TMO_TOTAL)
            return AIRY_ETIMEDOUT;
        return AIRY_ERR_IO;
    }

    *out_response = resp;
    *out_http_code = http_code;
    return AIRY_OK;
}

int provider_url_join(const char *api_base, const char *path, char *out, size_t out_size)
{
    if (!api_base || !path || !out || out_size == 0) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }

    size_t blen = strlen(api_base);
    while (blen > 0 && (api_base[blen - 1] == '/' || api_base[blen - 1] == '\\'))
        blen--;
    if (blen == 0) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }

    int n = snprintf(out, out_size, "%.*s%s", (int)blen, api_base, path);
    if (n < 0 || (size_t)n >= out_size) {
        errno = ENAMETOOLONG;
        return AIRY_ERR_INVALID_PARAM;
    }
    return AIRY_OK;
}

/* 单发 GET（不重试）：幂等只读请求。健康探测对同一不可达端点反复重试没有
 * 意义——它只会把"端点是否可用"的结论推迟到探测窗口之外，且探测本身运行
 * 在线程池 worker 上，重试会长期占用 worker。故此处有意不经 retry 壳。 */
int provider_http_get(const char *url, struct curl_slist *headers, double timeout_sec,
                      provider_http_resp_t **out_response, long *out_http_code)
{
    if (!url || !out_response || !out_http_code) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }

    provider_http_resp_t *resp = (provider_http_resp_t *)AIRY_CALLOC(1, sizeof(*resp));
    if (!resp)
        return AIRY_ERR_OUT_OF_MEMORY;

    CURL *curl = curl_easy_init();
    if (!curl) {
        AIRY_FREE(resp);
        return AIRY_ERR_IO;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    provider_http_setup(curl, timeout_sec, PROVIDER_READ_IDLE_SEC);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        SVC_LOG_DEBUG("C-L02: PROVIDER: HTTP-GET-FAIL url=%s diag=%s", url,
                      provider_http_diag(res));
        provider_http_resp_free(resp);
        return AIRY_ERR_IO;
    }

    *out_response = resp;
    *out_http_code = http_code;
    return AIRY_OK;
}

/* R-1 / N-3：provider HTTP 状态码 → airy 错误码归一（流式与非流式共用
 * SSoT）。401/403 归鉴权失败、429 归限流，避免笼统的"网络请求失败"掩盖
 * 根因；400/422 是 provider 明确拒绝请求体，归专用码以便跳过外层重试。
 * http_code == 0 表示请求未达上游，返回 fallback。 */
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

/* 端点附加头：配置注入的 "Name: value" 列表（';' 或换行分隔），逐条裁剪
 * 空白后并入头链；不含冒号或为空的行丢弃，避免把坏配置变成坏请求。 */
static struct curl_slist *headers_add_extra(struct curl_slist *headers, const char *raw)
{
    char buf[512];
    if (!raw || !raw[0])
        return headers;
    AIRY_STRNCPY_TERM(buf, raw, sizeof(buf));

    for (char *p = buf, *sep; p && *p; p = sep ? sep + 1 : NULL) {
        sep = strpbrk(p, ";\n");
        if (sep)
            *sep = '\0';
        while (*p == ' ' || *p == '\t')
            ++p;
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r'))
            p[--n] = '\0';
        if (*p && strchr(p, ':'))
            headers = curl_slist_append(headers, p);
    }
    return headers;
}

/* 头链装配 SSoT（B16-S3.1）：鉴权样式 + Content-Type + 端点附加头。适配层
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

    headers = headers_add_extra(headers, base->extra_headers);

    return headers;
}

double provider_budget_sec(const provider_base_ctx_t *base)
{
    if (!base)
        return PROVIDER_READ_IDLE_SEC;
    /* timeout_sec 为 0（自动预算端点）或 timeout_auto 置位时，均由实测
     * 时延 p99 推导；两者是同一条规则的两种表达，此处一并兜住。 */
    if (base->timeout_auto || base->timeout_sec <= 0.0)
        return prov_tmo_auto(base->name);
    return base->timeout_sec;
}

static int exec_args_ok(const provider_request_t *req, provider_http_resp_t *const *out_response,
                        const long *out_http_code, int streaming)
{
    if (!req || !req->base || !req->path || !req->body || !out_http_code)
        return 0;
    return streaming || out_response;
}

/* 出网形态四选一：SSE 流、裸流、带限流器的同步、普通同步。形态只由请求
 * 声明决定，与 URL/头/预算的装配无关，故独立于装配步骤。 */
static int exec_post(const provider_request_t *req, const char *url, struct curl_slist *headers,
                     double budget, provider_http_resp_t **out_response, long *out_http_code)
{
    if (req->on_event)
        return provider_http_post_stream_sse(url, headers, req->body, budget,
                                             req->base->max_retries, req->base->retry_class,
                                             req->idempotent, req->on_event, req->user_data,
                                             out_http_code);
    if (req->on_chunk)
        return provider_http_post_stream(url, headers, req->body, budget, req->base->max_retries,
                                         req->base->retry_class, req->idempotent, req->on_chunk,
                                         req->user_data, out_http_code);
    if (req->rl)
        return provider_http_request_with_retry(req->base, req->rl, url, headers, req->body, budget,
                                                req->idempotent, out_http_code, out_response);
    return provider_http_post(url, headers, req->body, budget, req->base->max_retries,
                              req->base->retry_class, req->idempotent, out_response, out_http_code);
}

int provider_http_exec(const provider_request_t *req, provider_http_resp_t **out_response,
                       long *out_http_code)
{
    int streaming = req && (req->on_event || req->on_chunk);
    if (!exec_args_ok(req, out_response, out_http_code, streaming)) {
        errno = EINVAL;
        return AIRY_ERR_INVALID_PARAM;
    }

    /* URL 拼装 SSoT（B16-S6）：api_base 尾部斜杠在此归一，调用方只声明
     * path（如 "/embeddings"），所有出网点共用 provider_url_join 一条规则。 */
    char url[1024];
    int join_rc = provider_url_join(req->base->api_base, req->path, url, sizeof(url));
    if (join_rc != AIRY_OK)
        return join_rc;

    struct curl_slist *headers = provider_headers_build(req->base, req->headers);

    /* 预算由请求侧决定（适配层按时延 p99 推导），未声明则回落 provider 预算。 */
    double budget = req->timeout_sec > 0.0 ? req->timeout_sec : provider_budget_sec(req->base);
    int rc = exec_post(req, url, headers, budget, out_response, out_http_code);

    if (headers)
        curl_slist_free_all(headers);
    return rc;
}

int prov_probe_get(const provider_base_ctx_t *base, const char *path,
                   const provider_header_spec_t *headers, double timeout_sec,
                   provider_http_resp_t **out_response, long *out_http_code)
{
    if (!base || !base->api_base[0] || !path || !out_http_code)
        return AIRY_ERR_INVALID_PARAM;

    char url[1024];
    int join_rc = provider_url_join(base->api_base, path, url, sizeof(url));
    if (join_rc != AIRY_OK)
        return join_rc;

    struct curl_slist *hdrs = provider_headers_build(base, headers);
    double budget = timeout_sec > 0.0 ? timeout_sec : PROVIDER_CONNECT_TIMEOUT_SEC;
    int rc = provider_http_get(url, hdrs, budget, out_response, out_http_code);
    if (hdrs)
        curl_slist_free_all(hdrs);
    return rc;
}
