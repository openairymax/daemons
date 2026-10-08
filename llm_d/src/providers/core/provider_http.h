/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_http.h: Provider 域唯一出网门面（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_HTTP_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_HTTP_H

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
    provider_stream_chunk_cb_t on_chunk;   /* 非流式留 NULL */
    provider_sse_event_cb_t on_event;      /* 非流式留 NULL */
    void *user_data;
} provider_request_t;

int provider_http_exec(const provider_request_t *req, provider_http_resp_t **out_response,
                       long *out_http_code);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_HTTP_H */
