// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"

#include "error.h"
/**
 * @file deepseek.c
 * @brief DeepSeek adapter（OpenAI 兼容协议）。
 *
 * B16-S3 c6：流式累积器与流末装配收敛 core/toolstream.c（与 openai/local
 * 共用 provider_stream_acc_t 四件套）；请求头装配收敛 provider_openai_
 * headers（无密钥省略 Authorization——R-1，修复旧实现拼空 Bearer 的
 * 同构漂移，部分网关会因空凭据返回难以定位的 400/401）。适配层只留
 * 厂商差异面（默认端点/模型、日志前缀）。
 */

#include "daemon_errors.h"
#include "daemon_platform_ext.h"
#include "core/adapter.h"
#include "core/secrets.h"
#include "core/toolstream.h"
#include "svc_logger.h"

#include <curl/curl.h>
#include <string.h>


#define DEEPSEEK_DEFAULT_BASE "https://api.deepseek.com/v1"
#define DEEPSEEK_DEFAULT_MODEL "deepseek-flash"

typedef struct {
    provider_base_ctx_t base;
} deepseek_ctx_t;

static provider_ctx_t *deepseek_init(const char *name __attribute__((unused)), const char *api_key,
                                     const char *api_base,
                                     const char *organization __attribute__((unused)),
                                     double timeout_sec, int max_retries)
{

    deepseek_ctx_t *ctx = (deepseek_ctx_t *)AIRY_CALLOC(1, sizeof(deepseek_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: DEEPSEEK: INIT-FAIL — OOM allocating ctx (size=%zu)",
                      sizeof(deepseek_ctx_t));
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    provider_base_init(&ctx->base, api_key, api_base, organization, timeout_sec, max_retries,
                       DEEPSEEK_DEFAULT_BASE);

    SVC_LOG_INFO("C-L02: DEEPSEEK: INIT api_base=%s model=%s timeout=%.1fs max_retries=%d "
                 "has_api_key=%d",
                 ctx->base.api_base, DEEPSEEK_DEFAULT_MODEL, timeout_sec, max_retries,
                 (api_key && api_key[0]) ? 1 : 0);

    return (provider_ctx_t *)ctx;
}

static void deepseek_destroy(provider_ctx_t *ctx_ptr)
{
    if (ctx_ptr) {
        SVC_LOG_DEBUG("C-L02: DEEPSEEK: DESTROY ctx=%p", (void *)ctx_ptr);
        AIRY_FREE(ctx_ptr);
    }
}

static int deepseek_complete(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                             llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !out_response) {
        SVC_LOG_ERROR("C-L02: DEEPSEEK: COMPLETE-FAIL — invalid params "
                      "ctx=%p manager=%p out=%p",
                      (void *)ctx_ptr, (void *)manager, (void *)out_response);
        return AIRY_ERR_INVALID_PARAM;
    }

    deepseek_ctx_t *ctx = (deepseek_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    provider_refresh_api_key(base);

    const char *model =
        (manager->model && manager->model[0]) ? manager->model : DEEPSEEK_DEFAULT_MODEL;

    SVC_LOG_INFO("C-L02: DEEPSEEK: COMPLETE-START model=%s msgs=%zu max_tokens=%d temp=%.2f "
                 "stream=%d",
                 model, manager->message_count, manager->max_tokens, manager->temperature,
                 manager->stream);

    char *req_body = provider_build_openai_request(manager, DEEPSEEK_DEFAULT_MODEL);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: DEEPSEEK: COMPLETE-FAIL — request body build failed (OOM) "
                      "model=%s",
                      model);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t req_body_len = strlen(req_body);
    (void)req_body_len;

    char url[1024];
    struct curl_slist *headers =
        provider_openai_headers(base, "/chat/completions", url, sizeof(url));

    provider_http_resp_t *http_resp = NULL;
    long http_code = 0;

    SVC_LOG_DEBUG("C-L02: DEEPSEEK: HTTP-POST url=%s body_len=%zu timeout=%.1fs retries=%d", url,
                  req_body_len, base->timeout_sec, base->max_retries);

    int ret = provider_http_post(url, headers, req_body, base->timeout_sec, base->max_retries,
                                 &http_resp, &http_code);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: DEEPSEEK: COMPLETE-FAIL — HTTP request failed "
                      "url=%s http_code=%ld ret=%d timeout=%.1fs "
                      "STACK: provider_http_post() → deepseek_complete()",
                      url, http_code, ret, base->timeout_sec);
        return ret;
    }

    if (http_code != 200) {
        size_t resp_body_len = (http_resp && http_resp->data) ? strlen(http_resp->data) : 0;
        SVC_LOG_ERROR("C-L02: DEEPSEEK: COMPLETE-FAIL — HTTP error "
                      "url=%s http_code=%ld resp_body_len=%zu "
                      "DIAGNOSIS: %s"
                      " BODY: %.300s",
                      url, http_code, resp_body_len, provider_http_err_diag(http_code),
                      (http_resp && http_resp->data) ? http_resp->data : "");
        provider_http_resp_free(http_resp);
        return provider_http_err_map(http_code, AIRY_ERR_IO);
    }

    size_t resp_body_len = (http_resp && http_resp->data) ? strlen(http_resp->data) : 0;
    SVC_LOG_DEBUG("C-L02: DEEPSEEK: HTTP-RESPONSE http_code=%ld resp_body_len=%zu", http_code,
                  resp_body_len);

    if (!http_resp || !http_resp->data || http_resp->data[0] == '\0') {
        SVC_LOG_ERROR("C-L02: DEEPSEEK: COMPLETE-FAIL — empty response body "
                      "http_code=%ld STACK: deepseek_complete()",
                      http_code);
        provider_http_resp_free(http_resp);
        return AIRY_ERR_IO;
    }

    ret = provider_parse_openai_response(http_resp->data, out_response);
    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: DEEPSEEK: COMPLETE-FAIL — response parse failed "
                      "ret=%d resp_body_len=%zu "
                      "STACK: provider_parse_openai_response() → deepseek_complete()",
                      ret, resp_body_len);
    } else if (*out_response) {
        SVC_LOG_INFO(
            "C-L02: DEEPSEEK: COMPLETE-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
            "finish_reason=%s",
            (*out_response)->model ? (*out_response)->model : "unknown",
            (*out_response)->prompt_tokens, (*out_response)->completion_tokens,
            (*out_response)->total_tokens,
            (*out_response)->finish_reason ? (*out_response)->finish_reason : "none");
    }

    provider_http_resp_free(http_resp);

    return ret;
}

/* 流式 completion：SSE 传输 → core 累积器（provider_openai_on_chunk）→
 * 流末装配（provider_openai_stream_take）。本函数只编排，不持协议状态。 */
static int deepseek_complete_stream(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                                    llm_stream_callback_t callback, void *user_data,
                                    llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !callback) {
        SVC_LOG_ERROR("C-L02: DEEPSEEK: STREAM-FAIL — invalid params "
                      "ctx=%p manager=%p callback=%p",
                      (void *)ctx_ptr, (void *)manager, (void *)(uintptr_t)callback);
        return AIRY_ERR_INVALID_PARAM;
    }

    deepseek_ctx_t *ctx = (deepseek_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    provider_refresh_api_key(base);

    const char *model =
        (manager->model && manager->model[0]) ? manager->model : DEEPSEEK_DEFAULT_MODEL;

    SVC_LOG_INFO("C-L02: DEEPSEEK: STREAM-START model=%s msgs=%zu max_tokens=%d temp=%.2f", model,
                 manager->message_count, manager->max_tokens, manager->temperature);

    llm_request_config_t stream_cfg = *manager;
    stream_cfg.stream = 1;

    char *req_body = provider_build_openai_request(&stream_cfg, DEEPSEEK_DEFAULT_MODEL);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: DEEPSEEK: STREAM-FAIL — request body build failed (OOM) "
                      "model=%s",
                      model);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    char url[1024];
    struct curl_slist *headers =
        provider_openai_headers(base, "/chat/completions", url, sizeof(url));

    provider_stream_acc_t acc;
    provider_stream_acc_init(&acc, callback, user_data);

    SVC_LOG_DEBUG("C-L02: DEEPSEEK: STREAM-HTTP-POST url=%s body_len=%zu timeout=%.1fs", url,
                  strlen(req_body), base->timeout_sec);

    long http_code = 0;
    int ret = provider_http_post_stream(url, headers, req_body, base->timeout_sec,
                                        base->max_retries, provider_openai_on_chunk, &acc,
                                        &http_code);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: DEEPSEEK: STREAM-FAIL url=%s http_code=%ld ret=%d DIAGNOSIS=%s",
                      url, http_code, ret, provider_http_err_diag(http_code));
        provider_stream_acc_free(&acc);
        return provider_http_err_map(http_code, ret);
    }

    llm_response_t *resp = provider_openai_stream_take(&acc);
    provider_tool_flush(&acc.tools, resp, callback, user_data);
    provider_stream_acc_free(&acc);

    if (resp) {
        SVC_LOG_INFO(
            "C-L02: DEEPSEEK: STREAM-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
            "finish_reason=%s acc_len=%zu",
            resp->model ? resp->model : "unknown", resp->prompt_tokens, resp->completion_tokens,
            resp->total_tokens, resp->finish_reason ? resp->finish_reason : "none",
            resp->choices && resp->choices[0].content ? strlen(resp->choices[0].content) : 0);
    } else {
        SVC_LOG_WARN("C-L02: DEEPSEEK: STREAM — null response built");
    }

    if (out_response)
        *out_response = resp;
    else if (resp)
        provider_response_free(resp);

    return AIRY_OK;
}

const provider_adapter_t deepseek_ops = {.init = deepseek_init,
                                         .destroy = deepseek_destroy,
                                         .complete = deepseek_complete,
                                         .complete_stream = deepseek_complete_stream,
                                         .name = "deepseek",
                                         .default_model = DEEPSEEK_DEFAULT_MODEL,
                                         .default_base_url = DEEPSEEK_DEFAULT_BASE};
