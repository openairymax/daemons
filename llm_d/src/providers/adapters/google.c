// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file google.c
 * @brief Google Gemini adapter implementation.
 *
 * Gemini API specifics:
 * 1. Auth via the x-goog-api-key header (not a Bearer token)
 * 2. Request format: contents array + systemInstruction
 * 3. Response format: candidates[].content.parts[].text
 * 4. SSE streaming via core/sse.c named-event mode (alt=sse)
 */

#include "daemon_errors.h"
#include "daemon_platform_ext.h"
#include "core/adapter.h"
#include "core/secrets.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define GOOGLE_DEFAULT_BASE "https://generativelanguage.googleapis.com/v1beta"
#define GOOGLE_DEFAULT_MODEL "gemini-2.0-flash"

typedef struct {
    provider_base_ctx_t base;
} google_ctx_t;

static provider_ctx_t *google_init(const char *name __attribute__((unused)), const char *api_key,
                                   const char *api_base,
                                   const char *organization __attribute__((unused)),
                                   double timeout_sec, int max_retries)
{
    SVC_LOG_INFO("C-L02: GOOGLE: INIT api_base=%s model=%s timeout=%.1fs retries=%d has_api_key=%d",
                 api_base ? api_base : GOOGLE_DEFAULT_BASE, GOOGLE_DEFAULT_MODEL, timeout_sec,
                 max_retries, (api_key && api_key[0]) ? 1 : 0);

    google_ctx_t *ctx = (google_ctx_t *)AIRY_CALLOC(1, sizeof(google_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: GOOGLE: INIT-FAIL reason=alloc_failed");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    provider_base_init(&ctx->base, api_key, api_base, organization, timeout_sec, max_retries,
                       GOOGLE_DEFAULT_BASE);

    SVC_LOG_INFO("C-L02: GOOGLE: INIT ctx=%p", (void *)ctx);
    return (provider_ctx_t *)ctx;
}

static void google_destroy(provider_ctx_t *ctx_ptr)
{
    SVC_LOG_INFO("C-L02: GOOGLE: DESTROY ctx=%p", (void *)ctx_ptr);
    if (ctx_ptr) {
        AIRY_FREE(ctx_ptr);
    }
}

static char *google_build_request(const llm_request_config_t *manager)
{
    if (!manager) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    cJSON *contents = cJSON_CreateArray();
    char *system_instruction_text = NULL;

    for (size_t i = 0; i < manager->message_count; ++i) {
        const char *role = manager->messages[i].role ? manager->messages[i].role : "user";
        const char *content = manager->messages[i].content ? manager->messages[i].content : "";

        if (strcmp(role, "system") == 0) {
            system_instruction_text = AIRY_STRDUP(content);
            continue;
        }

        cJSON *entry = cJSON_CreateObject();
        const char *gemini_role = (strcmp(role, "assistant") == 0) ? "model" : "user";
        cJSON_AddStringToObject(entry, "role", gemini_role);

        cJSON *parts = cJSON_CreateArray();
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", content);
        cJSON_AddItemToArray(parts, part);
        cJSON_AddItemToObject(entry, "parts", parts);
        cJSON_AddItemToArray(contents, entry);
    }

    cJSON_AddItemToObject(root, "contents", contents);

    if (system_instruction_text) {
        cJSON *si = cJSON_CreateObject();
        cJSON *si_parts = cJSON_CreateArray();
        cJSON *si_part = cJSON_CreateObject();
        cJSON_AddStringToObject(si_part, "text", system_instruction_text);
        cJSON_AddItemToArray(si_parts, si_part);
        cJSON_AddItemToObject(si, "parts", si_parts);
        cJSON_AddItemToObject(root, "systemInstruction", si);
        AIRY_FREE(system_instruction_text);
    }

    cJSON *gen_config = cJSON_CreateObject();
    if (manager->temperature > 0) {
        cJSON_AddNumberToObject(gen_config, "temperature", manager->temperature);
    }
    if (manager->max_tokens > 0) {
        cJSON_AddNumberToObject(gen_config, "maxOutputTokens", manager->max_tokens);
    }
    cJSON_AddItemToObject(root, "generationConfig", gen_config);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static int google_parse_response(const char *body, llm_response_t **out)
{
    if (!body || !out) {
        return AIRY_ERR_INVALID_PARAM;
    }

    CJSON_PARSE_GUARD(root, body, { return AIRY_ERR_PARSE_ERROR; });

    llm_response_t *resp = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp) {

        return AIRY_ERR_OUT_OF_MEMORY;
    }

    cJSON *candidates = cJSON_GetObjectItem(root, "candidates");
    if (cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) > 0) {
        cJSON *first = cJSON_GetArrayItem(candidates, 0);
        cJSON *content_obj = cJSON_GetObjectItem(first, "content");
        if (content_obj) {
            cJSON *parts = cJSON_GetObjectItem(content_obj, "parts");
            if (cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0) {
                cJSON *part0 = cJSON_GetArrayItem(parts, 0);
                cJSON *text = cJSON_GetObjectItem(part0, "text");
                if (cJSON_IsString(text) && text->valuestring) {
                    resp->choices = (llm_message_t *)AIRY_CALLOC(1, sizeof(llm_message_t));
                    if (resp->choices) {
                        char *role_copy = AIRY_STRDUP("assistant");
                        char *content_copy = AIRY_STRDUP(text->valuestring);
                        if (role_copy && content_copy) {
                            resp->choices[0].role = role_copy;
                            resp->choices[0].content = content_copy;
                            resp->choice_count = 1;
                        } else {
                            AIRY_FREE(role_copy);
                            AIRY_FREE(content_copy);
                            AIRY_FREE(resp->choices);
                            resp->choices = NULL;
                            resp->choice_count = 0;
                        }
                    } else {
                        resp->choice_count = 0;
                    }
                }
            }
        }

        cJSON *finish = cJSON_GetObjectItem(first, "finishReason");
        if (cJSON_IsString(finish) && finish->valuestring)
            resp->finish_reason = AIRY_STRDUP(llm_finish_reason_norm(finish->valuestring));
    }

    cJSON *model = cJSON_GetObjectItem(root, "modelVersion");
    if (cJSON_IsString(model) && model->valuestring) {
        resp->model = AIRY_STRDUP(model->valuestring);
    }

    cJSON *usage = cJSON_GetObjectItem(root, "usageMetadata");
    if (usage) {
        cJSON *pt = cJSON_GetObjectItem(usage, "promptTokenCount");
        cJSON *ct = cJSON_GetObjectItem(usage, "candidatesTokenCount");
        if (cJSON_IsNumber(pt))
            resp->prompt_tokens = (uint32_t)pt->valuedouble;
        if (cJSON_IsNumber(ct))
            resp->completion_tokens = (uint32_t)ct->valuedouble;
        resp->total_tokens = resp->prompt_tokens + resp->completion_tokens;
    }

    *out = resp;
    return AIRY_OK;
}

static int google_complete(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                           llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !out_response) {
        return AIRY_ERR_INVALID_PARAM;
    }

    google_ctx_t *ctx = (google_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    provider_refresh_api_key(base);

    const char *model =
        (manager->model && manager->model[0]) ? manager->model : GOOGLE_DEFAULT_MODEL;

    SVC_LOG_INFO(
        "C-L02: GOOGLE: COMPLETE-START model=%s msg_count=%zu max_tokens=%d temp=%.2f stream=%d",
        model, manager->message_count, manager->max_tokens, manager->temperature, manager->stream);

    char *req_body = google_build_request(manager);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: GOOGLE: COMPLETE-FAIL model=%s reason=build_request_failed", model);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    char url[1024];
    snprintf(url, sizeof(url), "%s/models/%s:generateContent", base->api_base, model);

    struct curl_slist *headers = NULL;
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "x-goog-api-key: %s",
             base->api_key[0] ? base->api_key : "");
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    explicit_bzero(auth_header, sizeof(auth_header));

    size_t body_len = strlen(req_body);
    SVC_LOG_INFO(
        "C-L02: GOOGLE: HTTP-POST url=%s body_len=%zu timeout=%.1fs retries=%d auth=x-goog-api-key",
        url, body_len, base->timeout_sec, base->max_retries);

    provider_http_resp_t *http_resp = NULL;
    long http_code = 0;

    int ret = provider_http_post(url, headers, req_body, base->timeout_sec, base->max_retries,
                                 &http_resp, &http_code);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    size_t resp_body_len = (http_resp && http_resp->data) ? strlen(http_resp->data) : 0;
    SVC_LOG_INFO("C-L02: GOOGLE: HTTP-RESPONSE http_code=%ld resp_body_len=%zu", http_code,
                 resp_body_len);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR(
            "C-L02: GOOGLE: COMPLETE-FAIL model=%s reason=http_request_failed http_code=%ld ret=%d",
            model, http_code, ret);
        SVC_LOG_ERROR("C-L02: GOOGLE: STACK: google_complete http_request_failed url=%s", url);
        return ret;
    }

    if (http_code != 200) {
        const char *diagnosis = "";
        switch (http_code) {
        case 401:
            diagnosis = "DIAGNOSIS: invalid_api_key_or_expired";
            break;
        case 403:
            diagnosis = "DIAGNOSIS: access_denied_or_quota_exceeded";
            break;
        case 429:
            diagnosis = "DIAGNOSIS: rate_limited";
            break;
        case 500:
            diagnosis = "DIAGNOSIS: google_internal_server_error";
            break;
        case 503:
            diagnosis = "DIAGNOSIS: google_service_unavailable_or_overloaded";
            break;
        default:
            diagnosis = "DIAGNOSIS: unexpected_http_error";
            break;
        }
        SVC_LOG_ERROR("C-L02: GOOGLE: COMPLETE-FAIL model=%s http_code=%ld %s resp_body=%s", model,
                      http_code, diagnosis,
                      (http_resp && http_resp->data) ? http_resp->data : "(null)");
        SVC_LOG_ERROR("C-L02: GOOGLE: STACK: google_complete http_error url=%s http_code=%ld", url,
                      http_code);
        provider_http_resp_free(http_resp);
        return AIRY_ERR_IO;
    }

    ret = google_parse_response(http_resp->data, out_response);
    provider_http_resp_free(http_resp);

    if (ret == AIRY_OK && *out_response) {
        llm_response_t *r = *out_response;
        SVC_LOG_INFO("C-L02: GOOGLE: COMPLETE-OK model=%s prompt_tokens=%u completion_tokens=%u "
                     "total_tokens=%u finish_reason=%s",
                     r->model ? r->model : model, r->prompt_tokens, r->completion_tokens,
                     r->total_tokens, r->finish_reason ? r->finish_reason : "unknown");
    } else {
        SVC_LOG_ERROR("C-L02: GOOGLE: COMPLETE-FAIL model=%s reason=parse_response_failed ret=%d",
                      model, ret);
        SVC_LOG_ERROR("C-L02: GOOGLE: STACK: google_complete parse_failed");
    }

    return ret;
}

typedef struct {
    llm_stream_callback_t user_cb;
    void *user_data;
    char *acc_content;
    size_t acc_cap;
    size_t acc_len;
    char *resp_model;
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    char *finish_reason;
} gg_stream_acc_t;

/* SSE 具名事件回调（core/sse.c 分帧后交付；data 恒 NUL 终结）。
 * google alt=sse 流只有 data: 行，event 恒为 NULL。 */
static int gg_feed_sse_data(const char *event __attribute__((unused)), const char *data,
                            size_t data_len, void *user_data)
{
    gg_stream_acc_t *acc = (gg_stream_acc_t *)user_data;

    if (!data || data_len == 0)
        return 0;

    CJSON_PARSE_GUARD(root, data, { return 0; });

    cJSON *candidates = cJSON_GetObjectItem(root, "candidates");
    if (cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) > 0) {
        cJSON *first = cJSON_GetArrayItem(candidates, 0);
        cJSON *content_obj = cJSON_GetObjectItem(first, "content");
        if (content_obj) {
            cJSON *parts = cJSON_GetObjectItem(content_obj, "parts");
            if (cJSON_IsArray(parts)) {
                for (int i = 0; i < cJSON_GetArraySize(parts); i++) {
                    cJSON *part = cJSON_GetArrayItem(parts, i);
                    cJSON *text = cJSON_GetObjectItem(part, "text");
                    if (cJSON_IsString(text) && text->valuestring) {
                        const char *chunk = text->valuestring;
                        size_t clen = strlen(chunk);

                        if (acc->user_cb)
                            acc->user_cb(chunk, acc->user_data);

                        if (clen > 0) {
                            size_t needed = acc->acc_len + clen + 1;
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
                            if (acc->acc_content && acc->acc_len + clen < acc->acc_cap) {
                                __builtin_memcpy(acc->acc_content + acc->acc_len, chunk, clen);
                                acc->acc_len += clen;
                                acc->acc_content[acc->acc_len] = '\0';
                            }
                        }
                    }
                }
            }
        }

        cJSON *finish = cJSON_GetObjectItem(first, "finishReason");
        if (cJSON_IsString(finish) && finish->valuestring) {
            AIRY_FREE(acc->finish_reason);
            acc->finish_reason = AIRY_STRDUP(llm_finish_reason_norm(finish->valuestring));
        }
    }

    cJSON *usage = cJSON_GetObjectItem(root, "usageMetadata");
    if (usage) {
        cJSON *pt = cJSON_GetObjectItem(usage, "promptTokenCount");
        cJSON *ct = cJSON_GetObjectItem(usage, "candidatesTokenCount");
        if (cJSON_IsNumber(pt))
            acc->prompt_tokens = (uint32_t)pt->valuedouble;
        if (cJSON_IsNumber(ct))
            acc->completion_tokens = (uint32_t)ct->valuedouble;
    }

    cJSON *mv = cJSON_GetObjectItem(root, "modelVersion");
    if (cJSON_IsString(mv) && mv->valuestring && !acc->resp_model) {
        acc->resp_model = AIRY_STRDUP(mv->valuestring);
    }

    return 0;
}

static llm_response_t *gg_build_stream_response(gg_stream_acc_t *acc)
{
    llm_response_t *r = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!r) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    r->id = AIRY_STRDUP("");
    r->model = acc->resp_model ? acc->resp_model : AIRY_STRDUP("unknown");
    acc->resp_model = NULL;
    r->prompt_tokens = acc->prompt_tokens;
    r->completion_tokens = acc->completion_tokens;
    r->total_tokens = r->prompt_tokens + r->completion_tokens;
    r->choices = (llm_message_t *)AIRY_CALLOC(1, sizeof(llm_message_t));
    if (r->choices) {
        char *role_copy = AIRY_STRDUP("assistant");
        if (role_copy) {
            r->choices[0].role = role_copy;
            r->choices[0].content = acc->acc_content;
            acc->acc_content = NULL;
            r->choice_count = 1;
        } else {
            AIRY_FREE(r->choices);
            r->choices = NULL;
            r->choice_count = 0;
        }
    } else {
        r->choice_count = 0;
    }
    r->finish_reason = acc->finish_reason ? acc->finish_reason : AIRY_STRDUP(LLM_FINISH_STOP);
    acc->finish_reason = NULL;
    return r;
}

static int google_complete_stream(provider_ctx_t *ctx_ptr, const llm_request_config_t *manager,
                                  llm_stream_callback_t callback, void *user_data,
                                  llm_response_t **out_response)
{
    if (!ctx_ptr || !manager || !callback)
        return AIRY_ERR_INVALID_PARAM;

    google_ctx_t *ctx = (google_ctx_t *)ctx_ptr;
    provider_base_ctx_t *base = &ctx->base;

    provider_refresh_api_key(base);

    const char *model =
        (manager->model && manager->model[0]) ? manager->model : GOOGLE_DEFAULT_MODEL;

    SVC_LOG_INFO(
        "C-L02: GOOGLE: STREAM-START model=%s msg_count=%zu max_tokens=%d temp=%.2f stream=%d",
        model, manager->message_count, manager->max_tokens, manager->temperature, manager->stream);

    llm_request_config_t stream_cfg = *manager;
    stream_cfg.stream = 1;

    char *req_body = google_build_request(&stream_cfg);
    if (!req_body) {
        SVC_LOG_ERROR("C-L02: GOOGLE: STREAM-FAIL model=%s reason=build_request_failed", model);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    char url[1024];
    snprintf(url, sizeof(url), "%s/models/%s:streamGenerateContent?alt=sse", base->api_base, model);

    struct curl_slist *headers = NULL;
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "x-goog-api-key: %s",
             base->api_key[0] ? base->api_key : "");
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    explicit_bzero(auth_header, sizeof(auth_header));

    size_t body_len = strlen(req_body);
    SVC_LOG_INFO("C-L02: GOOGLE: STREAM-HTTP-POST url=%s body_len=%zu timeout=%.1fs retries=%d "
                 "auth=x-goog-api-key sse=alt=sse",
                 url, body_len, base->timeout_sec, base->max_retries);

    gg_stream_acc_t acc;
    __builtin_memset(&acc, 0, sizeof(acc));
    acc.user_cb = callback;
    acc.user_data = user_data;
    acc.acc_cap = 4096;
    acc.acc_content = (char *)AIRY_MALLOC(acc.acc_cap);

    long http_code = 0;
    int ret = provider_http_post_stream_sse(url, headers, req_body, base->timeout_sec,
                                            base->max_retries, gg_feed_sse_data, &acc,
                                            &http_code);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: GOOGLE: STREAM-FAIL url=%s http_code=%ld ret=%d DIAGNOSIS=%s", url,
                      http_code, ret, provider_http_err_diag(http_code));
        SVC_LOG_ERROR(
            "C-L02: GOOGLE: STACK: google_complete_stream stream_failed url=%s http_code=%ld", url,
            http_code);
        AIRY_FREE(acc.acc_content);
        AIRY_FREE(acc.resp_model);
        AIRY_FREE(acc.finish_reason);
        return provider_http_err_map(http_code, ret);
    }

    llm_response_t *resp = gg_build_stream_response(&acc);
    AIRY_FREE(acc.acc_content);
    AIRY_FREE(acc.resp_model);
    AIRY_FREE(acc.finish_reason);

    if (resp) {
        SVC_LOG_INFO("C-L02: GOOGLE: STREAM-OK model=%s prompt_tokens=%u completion_tokens=%u "
                     "total_tokens=%u finish_reason=%s",
                     resp->model ? resp->model : model, resp->prompt_tokens,
                     resp->completion_tokens, resp->total_tokens,
                     resp->finish_reason ? resp->finish_reason : "unknown");
    } else {
        SVC_LOG_ERROR("C-L02: GOOGLE: STREAM-FAIL model=%s reason=build_response_failed", model);
        SVC_LOG_ERROR("C-L02: GOOGLE: STACK: google_complete_stream build_response_failed");
    }

    if (out_response)
        *out_response = resp;
    else if (resp)
        provider_response_free(resp);

    return AIRY_OK;
}

const provider_adapter_t google_ops = {.init = google_init,
                                   .destroy = google_destroy,
                                   .complete = google_complete,
                                   .complete_stream = google_complete_stream,
                                   .name = "google",
                                   .default_model = GOOGLE_DEFAULT_MODEL,
                                   .default_base_url = GOOGLE_DEFAULT_BASE};
