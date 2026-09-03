// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file openai.c
 * @brief OpenAI adapter：生命周期与非流式 completion。
 *
 * 域拆分（2026-08-27：原 820 行 → 3 文件 + internal 头）：
 * - openai.c            生命周期（init/destroy）、非流式 complete、openai_ops
 * - openai_rate_limit.c 令牌桶限流（RPM/TPM）、429 退避、带重试的 HTTP 请求
 * - openai_stream.c     SSE 流式 completion 与 tool-call 增量累积
 * - openai_internal.h   共享 ctx 布局、常量与跨文件函数声明
 *
 * 对外公共符号 openai_ops 与既有 provider 模式不变；本文件仍为
 * openai/deepseek/local 等 OpenAI 兼容 provider 的唯一收敛入口。
 */

#include "daemon_platform_ext.h"
#include "openai_internal.h"
#include "provider.h"
#include "svc_logger.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* macOS 严格 feature 宏（-std=c99 等）下 <string.h> 不声明 explicit_bzero，
 * Windows UCRT 亦无；此处 <string.h> 已包含（若有系统声明已就位，宏不会
 * 与其碰撞），随后所有调用点统一展开为 volatile 擦除，保证敏感内存清零
 * 不被优化器消去。 */
#ifndef explicit_bzero
static inline void airy_provider_explicit_bzero(void *s, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)s;
    while (n-- > 0) {
        *p++ = 0;
    }
}
#define explicit_bzero(s, n) airy_provider_explicit_bzero((s), (n))
#endif


static provider_ctx_t *openai_init(const char *name __attribute__((unused)), const char *api_key,
                                   const char *api_base, const char *organization,
                                   double timeout_sec, int max_retries)
{

    openai_ctx_t *ctx = (openai_ctx_t *)AIRY_CALLOC(1, sizeof(openai_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: OPENAI: INIT-FAIL reason=oom STACK: openai_init");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    provider_base_init(&ctx->base, api_key, api_base, organization, timeout_sec, max_retries,
                       OPENAI_DEFAULT_BASE);

    openai_rl_init(&ctx->rl);

    airy_random_init();

    SVC_LOG_INFO("C-L02: OPENAI: INIT api_base=%s timeout=%.1fs retries=%d has_api_key=%d "
                 "RPM=%d TPM=%ld",
                 ctx->base.api_base[0] ? ctx->base.api_base : OPENAI_DEFAULT_BASE,
                 ctx->base.timeout_sec, ctx->base.max_retries, ctx->base.api_key[0] ? 1 : 0,
                 OPENAI_DEFAULT_RPM, (long)OPENAI_DEFAULT_TPM);

    return (provider_ctx_t *)ctx;
}

static void openai_destroy(provider_ctx_t *ctx_ptr)
{
    if (ctx_ptr) {
        openai_ctx_t *ctx = (openai_ctx_t *)ctx_ptr;
        SVC_LOG_INFO("C-L02: OPENAI: DESTROY ctx=%p", (void *)ctx_ptr);
        openai_rl_destroy(&ctx->rl);
        AIRY_FREE(ctx_ptr);
    }
}

static int openai_complete(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                           llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !out_response) {
        return AIRY_ERR_INVALID_PARAM;
    }

    openai_ctx_t *ctx = (openai_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    provider_refresh_api_key(base);

    char *req_body = provider_build_openai_request(manager, OPENAI_DEFAULT_MODEL);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: OPENAI: COMPLETE-FAIL model=%s reason=build_request_oom "
                      "STACK: openai_complete",
                      manager->model ? manager->model : OPENAI_DEFAULT_MODEL);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    const char *model = manager->model && manager->model[0] ? manager->model : OPENAI_DEFAULT_MODEL;
    SVC_LOG_INFO("C-L02: OPENAI: COMPLETE-START model=%s msgs=%zu max_tokens=%d temp=%.2f "
                 "stream=%d",
                 model, manager->message_count, manager->max_tokens, manager->temperature,
                 manager->stream ? 1 : 0);

    char url[1024];
    snprintf(url, sizeof(url), "%s/chat/completions", base->api_base);

    struct curl_slist *headers = NULL;
    char auth_header[1024];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
             base->api_key[0] ? base->api_key : "");
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    explicit_bzero(auth_header, sizeof(auth_header));

    provider_http_resp_t *http_resp = NULL;
    long http_code = 0;

    int ret = openai_http_request_with_retry(ctx, url, headers, req_body, &http_code, &http_resp);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        if (http_code == 429) {
            SVC_LOG_ERROR("C-L02: OPENAI: COMPLETE-FAIL model=%s http_code=%ld "
                          "DIAGNOSIS=rate_limit_exhausted",
                          model, http_code);
        } else {
            SVC_LOG_ERROR("C-L02: OPENAI: COMPLETE-FAIL model=%s http_code=%ld "
                          "DIAGNOSIS=http_request_failed body=%.600s",
                          model, http_code,
                          http_resp && http_resp->data ? http_resp->data : "");
        }
        if (http_resp)
            provider_http_resp_free(http_resp);
        return ret;
    }

    ret = provider_parse_openai_response(http_resp->data, out_response);
    provider_http_resp_free(http_resp);

    if (ret == AIRY_OK && *out_response) {
        SVC_LOG_INFO(
            "C-L02: OPENAI: COMPLETE-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
            "http_code=%ld",
            (*out_response)->model ? (*out_response)->model : model, (*out_response)->prompt_tokens,
            (*out_response)->completion_tokens, (*out_response)->total_tokens, http_code);
    } else {
        SVC_LOG_ERROR("C-L02: OPENAI: COMPLETE-FAIL model=%s http_code=%ld "
                      "DIAGNOSIS=parse_response_failed ret=%d",
                      model, http_code, ret);
    }

    return ret;
}

const provider_ops_t openai_ops = {.init = openai_init,
                                   .destroy = openai_destroy,
                                   .complete = openai_complete,
                                   .complete_stream = openai_complete_stream,
                                   .name = "openai",
                                   .default_model = OPENAI_DEFAULT_MODEL,
                                   .default_base_url = OPENAI_DEFAULT_BASE};
