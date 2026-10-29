/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_http.h: Provider 域唯一出网门面（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_HTTP_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_HTTP_H

#include "provider_auth.h"
#include "provider_base.h"
#include "provider_stream.h"
#include "provider_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    provider_base_ctx_t *base;
    struct provider_rate_limiter *rl;      /* 可 NULL */
    const char *path;                      /* 追加在 api_base 之后 */
    const provider_header_spec_t *headers; /* 可 NULL = 无鉴权单 Content-Type */
    const char *body;
    double timeout_sec;                    /* 0 = 用 base 的预算；非 0 为本次请求预算 */
    int idempotent;                        /* 1 = 重复投递语义等价（可重试） */
    provider_stream_chunk_cb_t on_chunk;   /* 非流式留 NULL */
    provider_sse_event_cb_t on_event;      /* 非流式留 NULL */
    void *user_data;
} provider_request_t;

int provider_http_exec(const provider_request_t *req, provider_http_resp_t **out_response,
                       long *out_http_code);

/* 探测型 GET 出网（门面出口）：幂等只读、不重试。头链走与业务请求同一套
 * 鉴权装配，path 由调用方声明并在此与 api_base 拼装。 */
int prov_probe_get(const provider_base_ctx_t *base, const char *path,
                   const provider_header_spec_t *headers, double timeout_sec,
                   provider_http_resp_t **out_response, long *out_http_code);

/* 本次请求的墙钟预算：显式 timeout_sec 优先；base 为自动预算时由实测
 * 时延 p99 推导（floor 120s，无样本 180s）。返回值恒 > 0。 */
double provider_budget_sec(const provider_base_ctx_t *base);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_HTTP_H */
