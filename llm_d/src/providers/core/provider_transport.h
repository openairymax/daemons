/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_transport.h: Provider 域传输原语（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_TRANSPORT_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_TRANSPORT_H

#include <curl/curl.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} provider_http_resp_t;

/* 出网传输策略：连接/总超时、代理、自定义 CA、重定向与证书校验。 */
void provider_http_setup(CURL *curl, double timeout_sec);

/* 出网失败分诊：libcurl 错误码归为可判读类别。返回值恒非 NULL。 */
const char *provider_http_diag(CURLcode code);

int provider_http_post(const char *url, struct curl_slist *headers, const char *body,
                       double timeout_sec, int max_retries, provider_http_resp_t **out_response,
                       long *out_http_code);
void provider_http_resp_free(provider_http_resp_t *resp);

/* HTTP 状态码 → airy 错误码归一；未命中时返回 fallback。 */
int provider_http_err_map(long http_code, int fallback);

/* 状态码 → 日志诊断串。返回值恒非 NULL。 */
const char *provider_http_err_diag(long http_code);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_TRANSPORT_H */
