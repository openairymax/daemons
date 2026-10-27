/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_transport.h: Provider 域传输原语（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_TRANSPORT_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_TRANSPORT_H

#include "provider_retry_class.h"
#include "prov_stat.h"

#include <curl/curl.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 连接阶段预算与读空闲上限的缺省值；均可用环境变量覆盖
 * （AIRY_HTTP_CONNECT_TIMEOUT / AIRY_HTTP_READ_IDLE），便于现场调参。 */
#define PROVIDER_CONNECT_TIMEOUT_SEC 3.0
#define PROVIDER_READ_IDLE_SEC 30.0

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} provider_http_resp_t;

/* 出网传输策略：连接/总超时、读空闲、代理、自定义 CA、重定向与证书校验。 */
void provider_http_setup(CURL *curl, double timeout_sec, double read_idle_sec);

/* 超时成因归类（超时三拆的传输侧判据）：总耗时已贴近整轮预算判 TOTAL；
 * 连接尚未建立判 CONNECT（请求未送达）；其余判 STALL（连接已建立但上游
 * 停止产出）。非超时返回 PROV_TMO_KIND_N。 */
prov_tmo_t provider_tmo_class(CURL *curl, double budget_sec);

/* 出网失败分诊：libcurl 错误码归为可判读类别。返回值恒非 NULL。 */
const char *provider_http_diag(CURLcode code);

/* idempotent != 0 表示重复投递不会改变上游语义（temperature==0 且 seed 固定），
 * 仅此类请求允许在"请求可能已送达"的失败形态下重试。 */
int provider_http_post(const char *url, struct curl_slist *headers, const char *body,
                       double timeout_sec, int max_retries, provider_retry_class_t retry_class,
                       int idempotent, provider_http_resp_t **out_response, long *out_http_code);

/* 单发 GET（不重试）：幂等只读请求，用于端点健康探测。headers 可空。 */
int provider_http_get(const char *url, struct curl_slist *headers, double timeout_sec,
                      provider_http_resp_t **out_response, long *out_http_code);

void provider_http_resp_free(provider_http_resp_t *resp);

/* api_base 与 path 拼装 SSoT：归一 api_base 尾部斜杠后拼接（调用方只声明
 * path，如 "/models"）。result 不可容纳时返回 AIRY_ERR_INVALID_PARAM。 */
int provider_url_join(const char *api_base, const char *path, char *out, size_t out_size);

/* HTTP 状态码 → airy 错误码归一；未命中时返回 fallback。 */
int provider_http_err_map(long http_code, int fallback);

/* 状态码 → 日志诊断串。返回值恒非 NULL。 */
const char *provider_http_err_diag(long http_code);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_TRANSPORT_H */
