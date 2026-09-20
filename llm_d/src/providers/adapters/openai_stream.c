// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file openai_stream.c
 * @brief OpenAI 适配器 SSE 流式 completion。
 *
 * 域拆分自 openai.c（2026-08-27）：流式 tool-call 增量累积、usage 累计、
 * 流式响应组装与 openai_complete_stream。SSE 传输与控制帧发射复用
 * provider_stream.c 的公共实现（provider.h）。
 */

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "error.h"
#include "openai_internal.h"
#include "core/adapter.h"
#include "core/secrets.h"
#include "core/toolstream.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
    llm_stream_callback_t user_cb;
    void *user_data;
    char *acc_content;
    size_t acc_cap;
    size_t acc_len;
    char *acc_reasoning;
    size_t acc_reasoning_cap;
    size_t acc_reasoning_len;
    char *resp_id;
    char *resp_model;
    uint64_t resp_created;
    char *finish_reason;
    /* 2.1.1.5 修复：流式 usage 累计（OpenAI/DeepSeek 在末尾 chunk 附带
     * usage，此前完全不解析，流式 token 统计恒为 0）。 */
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;
    uint32_t reasoning_tokens;
    provider_tool_acc_t tools;
} oai_stream_acc_t;

static int oai_stream_on_chunk(const char *json_line, void *userdata)
{
    oai_stream_acc_t *acc = (oai_stream_acc_t *)userdata;

    CJSON_PARSE_GUARD(root, json_line, { return 0; });

    if (!acc->resp_id) {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsString(id) && id->valuestring) {
            acc->resp_id = AIRY_STRDUP(id->valuestring);
        }
    }

    if (!acc->resp_model) {
        cJSON *model = cJSON_GetObjectItem(root, "model");
        if (cJSON_IsString(model) && model->valuestring) {
            acc->resp_model = AIRY_STRDUP(model->valuestring);
        }
    }

    cJSON *created = cJSON_GetObjectItem(root, "created");
    if (cJSON_IsNumber(created) && acc->resp_created == 0) {
        acc->resp_created = (uint64_t)created->valuedouble;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *delta = cJSON_GetObjectItem(choice, "delta");
        if (delta) {
            cJSON *content = cJSON_GetObjectItem(delta, "content");
            if (cJSON_IsString(content) && content->valuestring) {
                const char *text = content->valuestring;

                if (acc->user_cb) {
                    acc->user_cb(text, acc->user_data);
                }

                char *grown =
                    provider_buf_append(acc->acc_content, &acc->acc_cap, &acc->acc_len, text);
                if (grown) {
                    acc->acc_content = grown;
                }
            }

            /* Reasoning trace arrives in the same delta stream (DeepSeek/
             * Kimi). Forward each delta immediately as an RS 'R' control
             * frame so IPC clients (gateway) can show the thinking chain
             * live (2026-08-17), while still accumulating it for the
             * assembled response (tool-loop echo). */
            cJSON *reasoning = cJSON_GetObjectItem(delta, "reasoning_content");
            if (cJSON_IsString(reasoning) && reasoning->valuestring) {
                if (acc->user_cb) {
                    provider_emit_reasoning_frame(acc->user_cb, acc->user_data,
                                             reasoning->valuestring);
                }
                char *grown =
                    provider_buf_append(acc->acc_reasoning, &acc->acc_reasoning_cap,
                                        &acc->acc_reasoning_len, reasoning->valuestring);
                if (grown) {
                    acc->acc_reasoning = grown;
                }
            }

            provider_tool_delta(&acc->tools, delta);
        }

        cJSON *fr = cJSON_GetObjectItem(choice, "finish_reason");
        if (cJSON_IsString(fr) && fr->valuestring && strcmp(fr->valuestring, "null") != 0) {
            AIRY_FREE(acc->finish_reason);
            acc->finish_reason = AIRY_STRDUP(llm_finish_reason_norm(fr->valuestring));
        }
    }

    /* 2.1.1.5 修复：流式末尾 chunk 携带 usage（OpenAI stream_options
     * include_usage / DeepSeek 默认在最后 chunk 附带；该 chunk 无 choices，
     * 仅 usage）。最后一次解析覆盖前面（usage 在流末尾最完整）。 */
    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
        cJSON *tt = cJSON_GetObjectItem(usage, "total_tokens");
        if (cJSON_IsNumber(pt))
            acc->prompt_tokens = (uint32_t)pt->valuedouble;
        if (cJSON_IsNumber(ct))
            acc->completion_tokens = (uint32_t)ct->valuedouble;
        if (cJSON_IsNumber(tt))
            acc->total_tokens = (uint32_t)tt->valuedouble;
        cJSON *rt = cJSON_GetObjectItem(usage, "reasoning_tokens");
        if (!cJSON_IsNumber(rt)) {
            cJSON *details = cJSON_GetObjectItem(usage, "completion_tokens_details");
            if (cJSON_IsObject(details))
                rt = cJSON_GetObjectItem(details, "reasoning_tokens");
        }
        if (cJSON_IsNumber(rt))
            acc->reasoning_tokens = (uint32_t)rt->valuedouble;
    }

    return 0;
}

static llm_response_t *oai_build_stream_response(oai_stream_acc_t *acc)
{
    llm_response_t *resp = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    if (acc->resp_id)
        resp->id = acc->resp_id;
    else
        resp->id = AIRY_STRDUP("");
    acc->resp_id = NULL;

    if (acc->resp_model)
        resp->model = acc->resp_model;
    else
        resp->model = AIRY_STRDUP("unknown");
    acc->resp_model = NULL;

    resp->created = acc->resp_created;

    resp->choices = (llm_message_t *)AIRY_CALLOC(1, sizeof(llm_message_t));
    if (resp->choices) {
        resp->choice_count = 1;
        resp->choices[0].role = AIRY_STRDUP("assistant");
        resp->choices[0].content = acc->acc_content;
        acc->acc_content = NULL;
        resp->choices[0].reasoning_content = acc->acc_reasoning;
        acc->acc_reasoning = NULL;
    } else {
        resp->choice_count = 0;
    }

    if (acc->finish_reason) {
        resp->finish_reason = acc->finish_reason;
        acc->finish_reason = NULL;
    } else {
        resp->finish_reason = AIRY_STRDUP(LLM_FINISH_STOP);
    }

    resp->prompt_tokens = acc->prompt_tokens;
    resp->completion_tokens = acc->completion_tokens;
    resp->total_tokens = acc->total_tokens;
    resp->reasoning_tokens = acc->reasoning_tokens;

    return resp;
}

int openai_complete_stream(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
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
    snprintf(url, sizeof(url), "%s/chat/completions", base->api_base);

    struct curl_slist *headers = NULL;
    if (base->api_key[0]) {
        char auth_header[1024];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", base->api_key);
        headers = curl_slist_append(headers, auth_header);
        explicit_bzero(auth_header, sizeof(auth_header));
    } else {
        /* R-1：同非流式路径，未配置密钥时省略 Authorization 头。 */
        SVC_LOG_WARN("C-L02: OPENAI: STREAM no API key configured, sending unauthenticated "
                     "request model=%s",
                     model);
    }
    headers = curl_slist_append(headers, "Content-Type: application/json");

    oai_stream_acc_t acc;
    __builtin_memset(&acc, 0, sizeof(acc));
    acc.user_cb = callback;
    acc.user_data = user_data;
    acc.acc_cap = 4096;
    acc.acc_content = (char *)AIRY_MALLOC(acc.acc_cap);

    long http_code = 0;
    int ret = provider_http_post_stream(url, headers, req_body, base->timeout_sec,
                                        base->max_retries, oai_stream_on_chunk, &acc, &http_code);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        /* R-1/N-3：HTTP 状态码 → 错误码与诊断串归一收口于 core/http.c。 */
        SVC_LOG_ERROR("C-L02: OPENAI: STREAM-FAIL model=%s http_code=%ld DIAGNOSIS=%s", model,
                      http_code, provider_http_err_diag(http_code));
        AIRY_FREE(acc.acc_content);
        AIRY_FREE(acc.acc_reasoning);
        AIRY_FREE(acc.resp_id);
        AIRY_FREE(acc.resp_model);
        AIRY_FREE(acc.finish_reason);
        provider_tool_free(&acc.tools);
        return provider_http_err_map(http_code, ret);
    }

    llm_response_t *resp = oai_build_stream_response(&acc);
    AIRY_FREE(acc.acc_content);
    AIRY_FREE(acc.acc_reasoning);
    AIRY_FREE(acc.resp_id);
    AIRY_FREE(acc.resp_model);
    AIRY_FREE(acc.finish_reason);

    /* Streaming reasoning 已随 delta 实时发增量 RS 'R' 帧（思考链实时可见）；
     * 不再在流末重复发完整帧（resp->reasoning_content 仍保留供 tool 续轮）。 */

    provider_tool_flush(&acc.tools, resp, callback, user_data);
    provider_tool_free(&acc.tools);

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
