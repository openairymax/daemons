// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file local.c
 * @brief Local-model adapter（OpenAI 兼容协议，无鉴权本地端点）。
 *
 * B16-S3 c6：流式累积器与流末装配收敛 core/toolstream.c（与 openai/
 * deepseek 共用 provider_stream_acc_t 四件套；本地端点不产
 * reasoning_content，core 对应分支自然旁路）。出网经 core/http.c
 * provider_http_exec（URL 拼装、头链生命周期全归 core）；请求头以
 * PROVIDER_AUTH_NONE 声明单 Content-Type——本地服务无鉴权是设计使然，
 * 故不进无密钥告警分支。适配层只留厂商差异面（默认端点/模型、超时、
 * 日志），且不含任何 curl_* 调用。
 */

#include "airy_memory.h"
#include "error.h"
#include "daemon_errors.h"
#include "daemon_platform_ext.h"
#include "core/adapter.h"
#include "core/toolstream.h"
#include "svc_logger.h"

#include <stdio.h>
#include <string.h>

/* 代码级默认端点：仅当调用方未传 base_url 时使用。部署标准配置
 * （model.yaml local provider / llm: 段）显式传 base_url —— Ollama 默认
 * http://localhost:11434，vLLM 走 OpenAI 兼容格式 8080/v1——实际以
 * yaml 传入值为准（2026-08-20 审查：代码默认与 yaml 默认的差异为
 * 文档级，非功能缺陷）。 */
#define LOCAL_DEFAULT_BASE "http://localhost:8080/v1"
#define LOCAL_DEFAULT_MODEL "gpt-3.5-turbo"
#define LOCAL_DEFAULT_TIMEOUT 60.0
#define LOCAL_CHAT_PATH "/chat/completions"

/* 请求头声明：本地端点无鉴权（设计使然），core 仅附加 Content-Type。 */
static const provider_header_spec_t LOCAL_HEADERS = {.auth = PROVIDER_AUTH_NONE};

typedef struct {
    provider_base_ctx_t base;
} local_ctx_t;

static provider_ctx_t *local_init(const char *name, const char *api_key __attribute__((unused)),
                                  const char *api_base,
                                  const char *organization __attribute__((unused)),
                                  double timeout_sec, int max_retries)
{

    local_ctx_t *ctx = (local_ctx_t *)AIRY_CALLOC(1, sizeof(local_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: LOCAL: INIT-FAIL — OOM allocating ctx (size=%zu)",
                      sizeof(local_ctx_t));
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    double timeout = timeout_sec > 0 ? timeout_sec : LOCAL_DEFAULT_TIMEOUT;
    provider_base_init(&ctx->base, name, NULL, api_base, NULL, timeout, max_retries,
                       LOCAL_DEFAULT_BASE);

    SVC_LOG_INFO("C-L02: LOCAL: INIT api_base=%s model=%s timeout=%.1fs max_retries=%d "
                 "has_api_key=%d (no auth, local endpoint, higher default timeout)",
                 ctx->base.api_base, LOCAL_DEFAULT_MODEL, timeout, max_retries, 0);

    return (provider_ctx_t *)ctx;
}

static void local_destroy(provider_ctx_t *ctx_ptr)
{
    if (ctx_ptr) {
        SVC_LOG_DEBUG("C-L02: LOCAL: DESTROY ctx=%p", (void *)ctx_ptr);
        AIRY_FREE(ctx_ptr);
    }
}

static int local_complete(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                          llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !out_response) {
        SVC_LOG_ERROR("C-L02: LOCAL: COMPLETE-FAIL — invalid params "
                      "ctx=%p manager=%p out=%p",
                      (void *)ctx_ptr, (void *)manager, (void *)out_response);
        return AIRY_ERR_INVALID_PARAM;
    }

    local_ctx_t *ctx = (local_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    const char *model =
        (manager->model && manager->model[0]) ? manager->model : LOCAL_DEFAULT_MODEL;

    SVC_LOG_INFO("C-L02: LOCAL: COMPLETE-START model=%s msgs=%zu max_tokens=%d temp=%.2f "
                 "stream=%d (no auth, local endpoint)",
                 model, manager->message_count, manager->max_tokens, manager->temperature,
                 manager->stream);

    char *req_body = provider_build_openai_request(manager, LOCAL_DEFAULT_MODEL);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: LOCAL: COMPLETE-FAIL — request body build failed (OOM) "
                      "model=%s",
                      model);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    provider_request_t req = {
        .base = base,
        .path = LOCAL_CHAT_PATH,
        .headers = &LOCAL_HEADERS,
        .body = req_body,
    };

    provider_http_resp_t *http_resp = NULL;
    long http_code = 0;

    int ret = provider_http_exec(&req, &http_resp, &http_code);

    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: LOCAL: COMPLETE-FAIL — HTTP request failed "
                      "api_base=%s http_code=%ld ret=%d timeout=%.1fs "
                      "STACK: provider_http_exec() → local_complete()",
                      base->api_base, http_code, ret, base->timeout_sec);
        return ret;
    }

    if (http_code != 200) {
        size_t resp_body_len = (http_resp && http_resp->data) ? strlen(http_resp->data) : 0;
        SVC_LOG_ERROR("C-L02: LOCAL: COMPLETE-FAIL — HTTP error "
                      "api_base=%s http_code=%ld resp_body_len=%zu DIAGNOSIS=%s BODY: %.300s",
                      base->api_base, http_code, resp_body_len, provider_http_err_diag(http_code),
                      (http_resp && http_resp->data) ? http_resp->data : "");
        provider_http_resp_free(http_resp);
        return provider_http_err_map(http_code, AIRY_ERR_IO);
    }

    ret = provider_parse_openai_response(http_resp->data, out_response);
    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: LOCAL: COMPLETE-FAIL — response parse failed "
                      "ret=%d STACK: provider_parse_openai_response() → local_complete()",
                      ret);
    } else if (*out_response) {
        SVC_LOG_INFO("C-L02: LOCAL: COMPLETE-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
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
static int local_complete_stream(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                                 llm_stream_callback_t callback, void *user_data,
                                 llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !callback) {
        SVC_LOG_ERROR("C-L02: LOCAL: STREAM-FAIL — invalid params "
                      "ctx=%p manager=%p callback=%p",
                      (void *)ctx_ptr, (void *)manager, (void *)(uintptr_t)callback);
        return AIRY_ERR_INVALID_PARAM;
    }

    local_ctx_t *ctx = (local_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    const char *model =
        (manager->model && manager->model[0]) ? manager->model : LOCAL_DEFAULT_MODEL;

    SVC_LOG_INFO("C-L02: LOCAL: STREAM-START model=%s msgs=%zu max_tokens=%d temp=%.2f "
                 "(no auth, local endpoint)",
                 model, manager->message_count, manager->max_tokens, manager->temperature);

    llm_request_config_t stream_cfg = *manager;
    stream_cfg.stream = 1;

    char *req_body = provider_build_openai_request(&stream_cfg, LOCAL_DEFAULT_MODEL);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: LOCAL: STREAM-FAIL — request body build failed (OOM) "
                      "model=%s",
                      model);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    provider_stream_acc_t acc;
    provider_stream_acc_init(&acc, callback, user_data);

    provider_request_t req = {
        .base = base,
        .path = LOCAL_CHAT_PATH,
        .headers = &LOCAL_HEADERS,
        .body = req_body,
        .on_chunk = provider_openai_on_chunk,
        .user_data = &acc,
    };

    long http_code = 0;
    int ret = provider_http_exec(&req, NULL, &http_code);

    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: LOCAL: STREAM-FAIL — HTTP stream error "
                      "api_base=%s http_code=%ld ret=%d DIAGNOSIS=%s",
                      base->api_base, http_code, ret, provider_http_err_diag(http_code));
        provider_stream_acc_free(&acc);
        return provider_http_err_map(http_code, ret);
    }

    llm_response_t *resp = provider_openai_stream_take(&acc);
    provider_tool_flush(&acc.tools, resp, callback, user_data);
    provider_stream_acc_free(&acc);

    if (resp) {
        SVC_LOG_INFO("C-L02: LOCAL: STREAM-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
                     "finish_reason=%s acc_len=%zu",
                     resp->model ? resp->model : "unknown", resp->prompt_tokens,
                     resp->completion_tokens, resp->total_tokens,
                     resp->finish_reason ? resp->finish_reason : "none",
                     resp->choices && resp->choices[0].content ? strlen(resp->choices[0].content) :
                                                                 0);
    } else {
        SVC_LOG_WARN("C-L02: LOCAL: STREAM — null response built");
    }

    if (out_response)
        *out_response = resp;
    else if (resp)
        provider_response_free(resp);

    return AIRY_OK;
}

const provider_adapter_t local_ops = {.init = local_init,
                                      .destroy = local_destroy,
                                      .complete = local_complete,
                                      .complete_stream = local_complete_stream,
                                      .name = "local",
                                      .default_model = LOCAL_DEFAULT_MODEL,
                                      .default_base_url = LOCAL_DEFAULT_BASE};
