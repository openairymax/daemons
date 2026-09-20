// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file openai.c
 * @brief OpenAI adapter：生命周期与非流式/流式 completion（三件合一）。
 *
 * B16-S3 c6：原 openai.c + openai_stream.c + openai_internal.h 三件合为
 * 本文件——按文件拆分曾是请求头装配、SSE 行协议、流末响应装配三重同构
 * 的载体。机制层已收敛：请求头走 core/http.c provider_openai_headers，
 * 行协议累积与流末装配走 core/toolstream.c（provider_stream_acc_t 四件
 * 套），适配层只保留厂商差异面（默认端点/模型、令牌桶限流、日志前缀）。
 *
 * 对外公共符号 openai_ops 不变。
 */

#include "airy_memory.h"
#include "error.h"
#include "daemon_platform_ext.h"
#include "core/adapter.h"
#include "core/rate_limit.h"
#include "core/secrets.h"
#include "core/toolstream.h"
#include "svc_logger.h"

#include <curl/curl.h>
#include <stdio.h>

#define OPENAI_DEFAULT_BASE "https://api.openai.com/v1"
#define OPENAI_DEFAULT_MODEL "gpt-3.5-turbo"

/* 运行时上下文：首字段必须为 provider_base_ctx_t（provider_base_ctx()
 * 依赖此布局）。令牌桶限流为 OpenAI 云端专属（RPM/TPM + 429 退避）。 */
typedef struct {
    provider_base_ctx_t base;
    provider_rate_limiter_t rl;
} openai_ctx_t;

static provider_ctx_t *openai_init(const char *name, const char *api_key, const char *api_base,
                                   const char *organization, double timeout_sec, int max_retries)
{
    openai_ctx_t *ctx = (openai_ctx_t *)AIRY_CALLOC(1, sizeof(openai_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: OPENAI: INIT-FAIL reason=oom STACK: openai_init");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    provider_base_init(&ctx->base, name, api_key, api_base, organization, timeout_sec, max_retries,
                       OPENAI_DEFAULT_BASE);

    provider_rl_init(&ctx->rl);

    airy_random_init();

    SVC_LOG_INFO("C-L02: OPENAI: INIT api_base=%s timeout=%.1fs retries=%d has_api_key=%d "
                 "RPM=%d TPM=%ld",
                 ctx->base.api_base[0] ? ctx->base.api_base : OPENAI_DEFAULT_BASE,
                 ctx->base.timeout_sec, ctx->base.max_retries, ctx->base.api_key[0] ? 1 : 0,
                 PROVIDER_RL_DEFAULT_RPM, (long)PROVIDER_RL_DEFAULT_TPM);

    return (provider_ctx_t *)ctx;
}

static void openai_destroy(provider_ctx_t *ctx_ptr)
{
    if (ctx_ptr) {
        openai_ctx_t *ctx = (openai_ctx_t *)ctx_ptr;
        SVC_LOG_INFO("C-L02: OPENAI: DESTROY ctx=%p", (void *)ctx_ptr);
        provider_rl_destroy(&ctx->rl);
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
    struct curl_slist *headers =
        provider_openai_headers(base, "/chat/completions", url, sizeof(url));

    provider_http_resp_t *http_resp = NULL;
    long http_code = 0;

    int ret =
        provider_http_request_with_retry(base, &ctx->rl, url, headers, req_body, &http_code,
                                         &http_resp);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        /* R-1/N-3：错误码已由 provider_http_request_with_retry 归一（core/http.c）；
         * 此处仅按状态码出诊断串 + 透传错误体。 */
        SVC_LOG_ERROR("C-L02: OPENAI: COMPLETE-FAIL model=%s http_code=%ld ret=%d DIAGNOSIS=%s "
                      "body=%.600s",
                      model, http_code, ret, provider_http_err_diag(http_code),
                      http_resp && http_resp->data ? http_resp->data : "");
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

/* 流式 completion：SSE 传输（core/sse.c）→ 行协议累积与增量转发
 * （core/toolstream.c provider_openai_on_chunk）→ 流末装配
 * （provider_openai_stream_take）。本函数只编排，不持任何协议状态。 */
static int openai_complete_stream(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                                  llm_stream_callback_t callback, void *user_data,
                                  llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !callback) {
        return AIRY_ERR_INVALID_PARAM;
    }

    openai_ctx_t *ctx = (openai_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    provider_refresh_api_key(base);

    llm_request_config_t stream_cfg = *manager;
    stream_cfg.stream = 1;

    char *req_body = provider_build_openai_request(&stream_cfg, OPENAI_DEFAULT_MODEL);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: OPENAI: STREAM-FAIL model=%s reason=build_request_oom "
                      "STACK: openai_complete_stream",
                      manager->model ? manager->model : OPENAI_DEFAULT_MODEL);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    const char *model = manager->model && manager->model[0] ? manager->model : OPENAI_DEFAULT_MODEL;
    SVC_LOG_INFO("C-L02: OPENAI: STREAM-START model=%s msgs=%zu max_tokens=%d temp=%.2f", model,
                 manager->message_count, manager->max_tokens, manager->temperature);

    char url[1024];
    struct curl_slist *headers =
        provider_openai_headers(base, "/chat/completions", url, sizeof(url));

    provider_stream_acc_t acc;
    provider_stream_acc_init(&acc, callback, user_data);

    long http_code = 0;
    int ret = provider_http_post_stream(url, headers, req_body, base->timeout_sec,
                                        base->max_retries, provider_openai_on_chunk, &acc,
                                        &http_code);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: OPENAI: STREAM-FAIL model=%s http_code=%ld DIAGNOSIS=%s", model,
                      http_code, provider_http_err_diag(http_code));
        provider_stream_acc_free(&acc);
        return provider_http_err_map(http_code, ret);
    }

    llm_response_t *resp = provider_openai_stream_take(&acc);
    provider_tool_flush(&acc.tools, resp, callback, user_data);
    provider_stream_acc_free(&acc);

    if (resp) {
        SVC_LOG_INFO("C-L02: OPENAI: STREAM-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
                     "http_code=%ld",
                     resp->model ? resp->model : model, resp->prompt_tokens,
                     resp->completion_tokens, resp->total_tokens, http_code);
    } else {
        SVC_LOG_WARN("C-L02: OPENAI: STREAM-FAIL model=%s http_code=%ld "
                     "DIAGNOSIS=null_response_built",
                     model, http_code);
    }

    if (out_response) {
        *out_response = resp;
    } else if (resp) {
        provider_response_free(resp);
    }

    return AIRY_OK;
}

const provider_adapter_t openai_ops = {.init = openai_init,
                                       .destroy = openai_destroy,
                                       .complete = openai_complete,
                                       .complete_stream = openai_complete_stream,
                                       .name = "openai",
                                       .default_model = OPENAI_DEFAULT_MODEL,
                                       .default_base_url = OPENAI_DEFAULT_BASE};
