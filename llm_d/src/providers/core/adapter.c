// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file adapter.c
 * @brief 通用 completion driver（B16-S3.3）：适配层四槽入口的唯一编排实现。
 *
 * 密钥刷新 → build_request（流式置 stream=1）→ provider_http_exec 出网
 * → 错误归一（provider_http_err_map / err_diag）→ parse_response 或流末
 * take+flush 装配 → 统一日志。适配器（adapters/）以两行 shim 传入自身
 * ops 调用本件，core 不出现任何厂商名分支——V16.4"core 零厂商特判"
 * 的结构保证。失败路径 http_resp 所有权：非限流失败时 provider_http_post
 * 已内部释放（指针保持 NULL）；限流失败 4xx 路径 resp 落地归调用方释放
 * ——统一防御式 free 覆盖两种模式。
 */

#include "airy_memory.h"
#include "error.h"
#include "adapter.h"
#include "secrets.h"
#include "toolstream.h"
#include "svc_logger.h"

int provider_driver_complete(provider_ctx_t *ctx, const provider_adapter_t *ops,
                             const llm_request_config_t *manager, llm_response_t **out_response)
{
    if (!ctx || !ops || !manager || !out_response || !ops->build_request || !ops->parse_response)
        return AIRY_ERR_INVALID_PARAM;

    provider_base_ctx_t *base = provider_base_ctx(ctx);
    provider_refresh_api_key(base);

    char *req_body = ops->build_request(manager);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: %s: COMPLETE-FAIL model=%s reason=build_request_oom "
                      "STACK: provider_driver_complete",
                      ops->tag, manager->model ? manager->model : ops->default_model);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    const char *model = (manager->model && manager->model[0]) ? manager->model : ops->default_model;
    SVC_LOG_INFO("C-L02: %s: COMPLETE-START model=%s msgs=%zu max_tokens=%d temp=%.2f stream=%d",
                 ops->tag, model, manager->message_count, manager->max_tokens,
                 manager->temperature, manager->stream ? 1 : 0);

    provider_request_t req = {
        .base = base,
        .rl = ops->rate_limiter ? ops->rate_limiter(ctx) : NULL,
        .path = ops->chat_path,
        .headers = ops->headers,
        .body = req_body,
    };

    provider_http_resp_t *http_resp = NULL;
    long http_code = 0;
    int ret = provider_http_exec(&req, &http_resp, &http_code);

    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: %s: COMPLETE-FAIL model=%s http_code=%ld ret=%d DIAGNOSIS=%s "
                      "body=%.600s",
                      ops->tag, model, http_code, ret, provider_http_err_diag(http_code),
                      http_resp && http_resp->data ? http_resp->data : "");
        if (http_resp)
            provider_http_resp_free(http_resp);
        return ret;
    }

    /* 非限流模式：exec 返回 OK 仅代表传输成功，成败看状态码（4xx/5xx 响应体
     * 对调用方有诊断价值，exec 不吞）；限流模式此分支不可达（非 200 已在
     * core 归一为错误码返回）。 */
    if (http_code != 200) {
        SVC_LOG_ERROR("C-L02: %s: COMPLETE-FAIL model=%s http_code=%ld DIAGNOSIS=%s body=%.600s",
                      ops->tag, model, http_code, provider_http_err_diag(http_code),
                      http_resp && http_resp->data ? http_resp->data : "");
        provider_http_resp_free(http_resp);
        return provider_http_err_map(http_code, AIRY_ERR_IO);
    }

    ret = ops->parse_response(http_resp->data, out_response);
    provider_http_resp_free(http_resp);

    if (ret == AIRY_OK && *out_response) {
        SVC_LOG_INFO("C-L02: %s: COMPLETE-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
                     "http_code=%ld",
                     ops->tag, (*out_response)->model ? (*out_response)->model : model,
                     (*out_response)->prompt_tokens, (*out_response)->completion_tokens,
                     (*out_response)->total_tokens, http_code);
    } else {
        SVC_LOG_ERROR("C-L02: %s: COMPLETE-FAIL model=%s http_code=%ld "
                      "DIAGNOSIS=parse_response_failed ret=%d",
                      ops->tag, model, http_code, ret);
    }

    return ret;
}

int provider_driver_complete_stream(provider_ctx_t *ctx, const provider_adapter_t *ops,
                                    const llm_request_config_t *manager,
                                    llm_stream_callback_t callback, void *callback_data,
                                    llm_response_t **out_response)
{
    if (!ctx || !ops || !manager || !callback || !ops->build_request)
        return AIRY_ERR_INVALID_PARAM;

    provider_base_ctx_t *base = provider_base_ctx(ctx);
    provider_refresh_api_key(base);

    llm_request_config_t stream_cfg = *manager;
    stream_cfg.stream = 1;

    char *req_body = ops->build_request(&stream_cfg);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: %s: STREAM-FAIL model=%s reason=build_request_oom "
                      "STACK: provider_driver_complete_stream",
                      ops->tag, manager->model ? manager->model : ops->default_model);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    const char *model = (manager->model && manager->model[0]) ? manager->model : ops->default_model;
    SVC_LOG_INFO("C-L02: %s: STREAM-START model=%s msgs=%zu max_tokens=%d temp=%.2f", ops->tag,
                 model, manager->message_count, manager->max_tokens, manager->temperature);

    provider_stream_acc_t acc;
    provider_stream_acc_init(&acc, callback, callback_data);

    /* 分帧模式由 feed_event 槽唯一决定：非 NULL 为具名事件流（anthropic），
     * NULL 为 OpenAI 行协议流（on_chunk 机制件）。 */
    provider_request_t req = {
        .base = base,
        .path = ops->chat_path,
        .headers = ops->headers,
        .body = req_body,
        .on_chunk = ops->feed_event ? NULL : provider_openai_on_chunk,
        .on_event = ops->feed_event,
        .user_data = &acc,
    };

    long http_code = 0;
    int ret = provider_http_exec(&req, NULL, &http_code);

    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: %s: STREAM-FAIL model=%s http_code=%ld DIAGNOSIS=%s", ops->tag,
                      model, http_code, provider_http_err_diag(http_code));
        provider_stream_acc_free(&acc);
        return provider_http_err_map(http_code, ret);
    }

    llm_response_t *resp = provider_openai_stream_take(&acc);
    provider_tool_flush(&acc.tools, resp, callback, callback_data);
    provider_stream_acc_free(&acc);

    if (resp) {
        SVC_LOG_INFO("C-L02: %s: STREAM-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
                     "http_code=%ld",
                     ops->tag, resp->model ? resp->model : model, resp->prompt_tokens,
                     resp->completion_tokens, resp->total_tokens, http_code);
    } else {
        SVC_LOG_WARN("C-L02: %s: STREAM-FAIL model=%s http_code=%ld DIAGNOSIS=null_response_built",
                     ops->tag, model, http_code);
    }

    if (out_response)
        *out_response = resp;
    else if (resp)
        provider_response_free(resp);

    return AIRY_OK;
}
