// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file anthropic.c
 * @brief Anthropic adapter：Messages API 的请求构造、响应解析与 SSE 事件累积。
 *
 * B16-S3：出网经 core/http.c provider_http_exec（URL 拼装、头链生命周期、
 * 重试全归 core），鉴权以 PROVIDER_AUTH_X_API_KEY 声明、anthropic-version
 * 走静态附加头，故本件不含任何 curl_* 调用。厂商差异面为：system 提取、
 * content 块数组（tool_use / tool_result）、具名事件流（message_start /
 * content_block_delta / message_delta）、stop_reason 归一。
 */

#include "airy_memory.h"
#include "error.h"
#include "daemon_errors.h"
#include "daemon_platform_ext.h"
#include "core/adapter.h"
#include "core/secrets.h"
#include "core/toolstream.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANTHROPIC_DEFAULT_BASE "https://api.anthropic.com/v1"
#define ANTHROPIC_DEFAULT_MODEL "claude-3-sonnet-20240229"
#define ANTHROPIC_CHAT_PATH "/messages"

/* 请求头声明：x-api-key 鉴权 + 协议版本附加头（Messages API 硬性要求）。 */
static const char *const ANTHROPIC_EXTRA[] = {"anthropic-version: 2023-06-01"};
static const provider_header_spec_t ANTHROPIC_HEADERS = {.auth = PROVIDER_AUTH_X_API_KEY,
                                                         .extra = ANTHROPIC_EXTRA,
                                                         .extra_count = 1};

typedef struct {
    provider_base_ctx_t base;
} anthropic_ctx_t;

static provider_ctx_t *anthropic_init(const char *name, const char *api_key, const char *api_base,
                                      const char *organization,
                                      double timeout_sec, int max_retries)
{

    anthropic_ctx_t *ctx = (anthropic_ctx_t *)AIRY_CALLOC(1, sizeof(anthropic_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: ANTHROPIC: INIT-FAIL — OOM allocating ctx (size=%zu)",
                      sizeof(anthropic_ctx_t));
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    provider_base_init(&ctx->base, name, api_key, api_base, organization, timeout_sec, max_retries,
                       ANTHROPIC_DEFAULT_BASE);

    SVC_LOG_INFO("C-L02: ANTHROPIC: INIT api_base=%s model=%s timeout=%.1fs max_retries=%d "
                 "has_api_key=%d",
                 ctx->base.api_base, ANTHROPIC_DEFAULT_MODEL, timeout_sec, max_retries,
                 (api_key && api_key[0]) ? 1 : 0);

    return (provider_ctx_t *)ctx;
}

static void anthropic_destroy(provider_ctx_t *ctx_ptr)
{
    if (ctx_ptr) {
        SVC_LOG_DEBUG("C-L02: ANTHROPIC: DESTROY ctx=%p", (void *)ctx_ptr);
        AIRY_FREE(ctx_ptr);
    }
}

static char *anthropic_build_request(const llm_request_config_t *manager)
{
    if (!manager) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    cJSON_AddStringToObject(root, "model",
                            manager->model && manager->model[0] ? manager->model :
                                                                  ANTHROPIC_DEFAULT_MODEL);
    cJSON_AddNumberToObject(root, "temperature",
                            manager->temperature > 0 ? manager->temperature : 0.7);

    if (manager->max_tokens > 0) {
        cJSON_AddNumberToObject(root, "max_tokens", manager->max_tokens);
    }

    if (manager->stream) {
        cJSON_AddBoolToObject(root, "stream", 1);
    }

    char *system_prompt = NULL;
    cJSON *messages = cJSON_CreateArray();

    for (size_t i = 0; i < manager->message_count; ++i) {
        if (manager->messages[i].role && strcmp(manager->messages[i].role, "system") == 0) {
            system_prompt =
                AIRY_STRDUP(manager->messages[i].content ? manager->messages[i].content : "");
        } else {
            cJSON *msg = cJSON_CreateObject();
            const char *role = manager->messages[i].role ? manager->messages[i].role : "user";
            const char *content = manager->messages[i].content ? manager->messages[i].content : "";
            cJSON_AddStringToObject(msg, "role", role);
            cJSON_AddStringToObject(msg, "content", content);
            cJSON_AddItemToArray(messages, msg);
        }
    }

    if (system_prompt) {
        cJSON_AddStringToObject(root, "system", system_prompt);
        AIRY_FREE(system_prompt);
    }

    cJSON_AddItemToObject(root, "messages", messages);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static int anthropic_parse_response(const char *body, llm_response_t **out)
{
    if (!body || !out) {
        return AIRY_ERR_INVALID_PARAM;
    }

    CJSON_PARSE_GUARD(root, body, { return AIRY_ERR_PARSE_ERROR; });

    llm_response_t *resp = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp) {

        return AIRY_ERR_OUT_OF_MEMORY;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id) && id->valuestring) {
        resp->id = AIRY_STRDUP(id->valuestring);
    }

    cJSON *model = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(model) && model->valuestring) {
        resp->model = AIRY_STRDUP(model->valuestring);
    }

    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (cJSON_IsArray(content) && cJSON_GetArraySize(content) > 0) {
        cJSON *first = cJSON_GetArrayItem(content, 0);
        cJSON *text = cJSON_GetObjectItem(first, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            resp->choices = (llm_message_t *)AIRY_CALLOC(1, sizeof(llm_message_t));
            if (!resp->choices) {
                resp->choice_count = 0;

                provider_response_free(resp);
                return AIRY_ERR_OUT_OF_MEMORY;
            }
            resp->choice_count = 1;
            resp->choices[0].role = AIRY_STRDUP("assistant");
            resp->choices[0].content = AIRY_STRDUP(text->valuestring);
        }
    }

    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (usage) {
        cJSON *input = cJSON_GetObjectItem(usage, "input_tokens");
        cJSON *output = cJSON_GetObjectItem(usage, "output_tokens");
        if (cJSON_IsNumber(input))
            resp->prompt_tokens = (uint32_t)input->valuedouble;
        if (cJSON_IsNumber(output))
            resp->completion_tokens = (uint32_t)output->valuedouble;
        resp->total_tokens = resp->prompt_tokens + resp->completion_tokens;
    }

    cJSON *stop = cJSON_GetObjectItem(root, "stop_reason");
    resp->finish_reason =
        AIRY_STRDUP(llm_finish_reason_norm(cJSON_IsString(stop) ? stop->valuestring : NULL));

    *out = resp;
    return AIRY_OK;
}

static int anthropic_complete(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                              llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !out_response) {
        SVC_LOG_ERROR("C-L02: ANTHROPIC: COMPLETE-FAIL — invalid params "
                      "ctx=%p manager=%p out=%p",
                      (void *)ctx_ptr, (void *)manager, (void *)out_response);
        return AIRY_ERR_INVALID_PARAM;
    }

    anthropic_ctx_t *ctx = (anthropic_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    provider_refresh_api_key(base);

    const char *model =
        (manager->model && manager->model[0]) ? manager->model : ANTHROPIC_DEFAULT_MODEL;

    SVC_LOG_INFO("C-L02: ANTHROPIC: COMPLETE-START model=%s msgs=%zu max_tokens=%d temp=%.2f "
                 "stream=%d",
                 model, manager->message_count, manager->max_tokens, manager->temperature,
                 manager->stream);

    char *req_body = anthropic_build_request(manager);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: ANTHROPIC: COMPLETE-FAIL — request body build failed "
                      "model=%s",
                      model);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    provider_request_t req = {
        .base = base,
        .path = ANTHROPIC_CHAT_PATH,
        .headers = &ANTHROPIC_HEADERS,
        .body = req_body,
    };

    provider_http_resp_t *http_resp = NULL;
    long http_code = 0;

    SVC_LOG_DEBUG("C-L02: ANTHROPIC: HTTP-POST api_base=%s body_len=%zu timeout=%.1fs "
                  "retries=%d",
                  base->api_base, strlen(req_body), base->timeout_sec, base->max_retries);

    int ret = provider_http_exec(&req, &http_resp, &http_code);

    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: ANTHROPIC: COMPLETE-FAIL — HTTP request failed "
                      "api_base=%s http_code=%ld ret=%d timeout=%.1fs "
                      "STACK: provider_http_exec() → anthropic_complete()",
                      base->api_base, http_code, ret, base->timeout_sec);
        return ret;
    }

    if (http_code != 200) {
        SVC_LOG_ERROR("C-L02: ANTHROPIC: COMPLETE-FAIL — HTTP error "
                      "api_base=%s http_code=%ld resp_body_len=%zu DIAGNOSIS=%s",
                      base->api_base, http_code,
                      (http_resp && http_resp->data) ? strlen(http_resp->data) : 0,
                      provider_http_err_diag(http_code));
        provider_http_resp_free(http_resp);
        return provider_http_err_map(http_code, AIRY_ERR_IO);
    }

    size_t resp_body_len = http_resp ? strlen(http_resp->data) : 0;
    SVC_LOG_DEBUG("C-L02: ANTHROPIC: HTTP-RESPONSE http_code=%ld resp_body_len=%zu", http_code,
                  resp_body_len);

    ret = anthropic_parse_response(http_resp->data, out_response);
    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: ANTHROPIC: COMPLETE-FAIL — response parse failed "
                      "ret=%d resp_body_len=%zu "
                      "STACK: anthropic_parse_response() → anthropic_complete()",
                      ret, resp_body_len);
    } else if (*out_response) {
        SVC_LOG_INFO(
            "C-L02: ANTHROPIC: COMPLETE-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
            "finish_reason=%s",
            (*out_response)->model ? (*out_response)->model : "unknown",
            (*out_response)->prompt_tokens, (*out_response)->completion_tokens,
            (*out_response)->total_tokens,
            (*out_response)->finish_reason ? (*out_response)->finish_reason : "none");
    }

    provider_http_resp_free(http_resp);

    return ret;
}

typedef struct {
    llm_stream_callback_t user_cb;
    void *user_data;
    char *acc_content;
    size_t acc_cap;
    size_t acc_len;
    char *resp_id;
    char *resp_model;
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    char *finish_reason;
} ant_stream_acc_t;

/* SSE 具名事件回调（core/sse.c 双行解析后交付；data 恒 NUL 终结）。
 * anthropic 事件语义在 data JSON 的 type 字段，event 行仅冗余。 */
static int ant_feed_sse_event(const char *event __attribute__((unused)), const char *data,
                              size_t data_len, void *user_data)
{
    ant_stream_acc_t *acc = (ant_stream_acc_t *)user_data;

    if (!data || data_len == 0)
        return 0;

    CJSON_PARSE_GUARD(root, data, { return 0; });

    const char *type_str = NULL;
    cJSON *type_field = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type_field))
        type_str = type_field->valuestring;

    if (type_str && strcmp(type_str, "message_start") == 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        if (msg) {
            cJSON *id = cJSON_GetObjectItem(msg, "id");
            if (cJSON_IsString(id) && id->valuestring)
                acc->resp_id = AIRY_STRDUP(id->valuestring);
            cJSON *model = cJSON_GetObjectItem(msg, "model");
            if (cJSON_IsString(model) && model->valuestring)
                acc->resp_model = AIRY_STRDUP(model->valuestring);
            cJSON *usage = cJSON_GetObjectItem(msg, "usage");
            if (usage) {
                cJSON *iptok = cJSON_GetObjectItem(usage, "input_tokens");
                if (cJSON_IsNumber(iptok))
                    acc->prompt_tokens = (uint32_t)iptok->valuedouble;
            }
        }
    } else if (type_str && strcmp(type_str, "content_block_delta") == 0) {
        cJSON *delta = cJSON_GetObjectItem(root, "delta");
        if (delta) {
            cJSON *dtype = cJSON_GetObjectItem(delta, "type");
            cJSON *dtext = cJSON_GetObjectItem(delta, "text");
            if (cJSON_IsString(dtype) && strcmp(dtype->valuestring, "text_delta") == 0 &&
                cJSON_IsString(dtext) && dtext->valuestring) {
                const char *text = dtext->valuestring;
                size_t tlen = strlen(text);

                if (acc->user_cb)
                    acc->user_cb(text, acc->user_data);

                if (tlen > 0) {
                    size_t needed = acc->acc_len + tlen + 1;
                    if (needed > acc->acc_cap) {
                        size_t nc = acc->acc_cap * 2;
                        while (nc < needed)
                            nc *= 2;
                        char *p = (char *)AIRY_REALLOC(acc->acc_content, nc);
                        if (p) {
                            acc->acc_content = p;
                            acc->acc_cap = nc;
                        }
                    }
                    if (acc->acc_content && acc->acc_len + tlen < acc->acc_cap) {
                        __builtin_memcpy(acc->acc_content + acc->acc_len, text, tlen);
                        acc->acc_len += tlen;
                        acc->acc_content[acc->acc_len] = '\0';
                    }
                }
            }
        }
    } else if (type_str && strcmp(type_str, "message_delta") == 0) {
        cJSON *delta = cJSON_GetObjectItem(root, "delta");
        if (delta) {
            cJSON *fr = cJSON_GetObjectItem(delta, "stop_reason");
            if (cJSON_IsString(fr) && fr->valuestring) {
                AIRY_FREE(acc->finish_reason);
                acc->finish_reason = AIRY_STRDUP(llm_finish_reason_norm(fr->valuestring));
            }
        }
        cJSON *usage = cJSON_GetObjectItem(root, "usage");
        if (usage) {
            cJSON *otok = cJSON_GetObjectItem(usage, "output_tokens");
            if (cJSON_IsNumber(otok))
                acc->completion_tokens = (uint32_t)otok->valuedouble;
        }
    }

    return 0;
}

static llm_response_t *ant_build_stream_response(ant_stream_acc_t *acc)
{
    llm_response_t *r = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!r) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    r->id = acc->resp_id ? acc->resp_id : AIRY_STRDUP("");
    acc->resp_id = NULL;
    r->model = acc->resp_model ? acc->resp_model : AIRY_STRDUP("unknown");
    acc->resp_model = NULL;
    r->prompt_tokens = acc->prompt_tokens;
    r->completion_tokens = acc->completion_tokens;
    r->total_tokens = r->prompt_tokens + r->completion_tokens;
    r->choices = (llm_message_t *)AIRY_CALLOC(1, sizeof(llm_message_t));
    if (r->choices) {
        r->choice_count = 1;
        r->choices[0].role = AIRY_STRDUP("assistant");
        r->choices[0].content = acc->acc_content;
        acc->acc_content = NULL;
    } else {
        r->choice_count = 0;
    }
    r->finish_reason = acc->finish_reason ? acc->finish_reason : AIRY_STRDUP(LLM_FINISH_STOP);
    acc->finish_reason = NULL;
    return r;
}

static int anthropic_complete_stream(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                                     llm_stream_callback_t callback, void *user_data,
                                     llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !callback) {
        SVC_LOG_ERROR("C-L02: ANTHROPIC: STREAM-FAIL — invalid params "
                      "ctx=%p manager=%p callback=%p",
                      (void *)ctx_ptr, (void *)manager, (void *)(uintptr_t)callback);
        return AIRY_ERR_INVALID_PARAM;
    }

    anthropic_ctx_t *ctx = (anthropic_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    provider_refresh_api_key(base);

    const char *model =
        (manager->model && manager->model[0]) ? manager->model : ANTHROPIC_DEFAULT_MODEL;

    SVC_LOG_INFO("C-L02: ANTHROPIC: STREAM-START model=%s msgs=%zu max_tokens=%d temp=%.2f", model,
                 manager->message_count, manager->max_tokens, manager->temperature);

    llm_request_config_t stream_cfg = *manager;
    stream_cfg.stream = 1;

    char *req_body = anthropic_build_request(&stream_cfg);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: ANTHROPIC: STREAM-FAIL — request body build failed "
                      "model=%s",
                      model);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    ant_stream_acc_t acc;
    __builtin_memset(&acc, 0, sizeof(acc));
    acc.user_cb = callback;
    acc.user_data = user_data;
    acc.acc_cap = 4096;
    acc.acc_content = (char *)AIRY_MALLOC(acc.acc_cap);

    provider_request_t req = {
        .base = base,
        .path = ANTHROPIC_CHAT_PATH,
        .headers = &ANTHROPIC_HEADERS,
        .body = req_body,
        .on_event = ant_feed_sse_event,
        .user_data = &acc,
    };

    SVC_LOG_DEBUG("C-L02: ANTHROPIC: STREAM-HTTP-POST api_base=%s body_len=%zu timeout=%.1fs",
                  base->api_base, strlen(req_body), base->timeout_sec);

    long http_code = 0;
    int ret = provider_http_exec(&req, NULL, &http_code);

    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: ANTHROPIC: STREAM-FAIL api_base=%s http_code=%ld ret=%d "
                      "DIAGNOSIS=%s",
                      base->api_base, http_code, ret, provider_http_err_diag(http_code));
        AIRY_FREE(acc.acc_content);
        AIRY_FREE(acc.resp_id);
        AIRY_FREE(acc.resp_model);
        AIRY_FREE(acc.finish_reason);
        return provider_http_err_map(http_code, ret);
    }

    llm_response_t *resp = ant_build_stream_response(&acc);
    AIRY_FREE(acc.acc_content);
    AIRY_FREE(acc.resp_id);
    AIRY_FREE(acc.resp_model);
    AIRY_FREE(acc.finish_reason);

    if (resp) {
        SVC_LOG_INFO(
            "C-L02: ANTHROPIC: STREAM-OK model=%s tokens=(prompt=%u,completion=%u,total=%u) "
            "finish_reason=%s acc_len=%zu",
            resp->model ? resp->model : "unknown", resp->prompt_tokens, resp->completion_tokens,
            resp->total_tokens, resp->finish_reason ? resp->finish_reason : "none",
            resp->choices && resp->choices[0].content ? strlen(resp->choices[0].content) : 0);
    } else {
        SVC_LOG_WARN("C-L02: ANTHROPIC: STREAM — null response built");
    }

    if (out_response)
        *out_response = resp;
    else if (resp)
        provider_response_free(resp);

    return AIRY_OK;
}

const provider_adapter_t anthropic_ops = {.init = anthropic_init,
                                      .destroy = anthropic_destroy,
                                      .complete = anthropic_complete,
                                      .complete_stream = anthropic_complete_stream,
                                      .name = "anthropic",
                                      .default_model = ANTHROPIC_DEFAULT_MODEL,
                                      .default_base_url = ANTHROPIC_DEFAULT_BASE};
