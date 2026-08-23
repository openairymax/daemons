// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include "error.h"
/**
 * @file local.c
 * @brief Local-model adapter (OpenAI-compatible format).
 *
 * Improvements:
 * 1. Uses the common Provider infrastructure
 * 2. Code reduced from ~370 to ~140 lines
 * 3. Duplication with openai.c/deepseek.c eliminated
 */

#include "daemon_errors.h"
#include "daemon_platform_ext.h"
#include "provider.h"
#include "svc_logger.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 代码级默认端点：仅当调用方未传 base_url 时使用。部署标准配置
 * （model.yaml local provider / llm: 段）显式传 base_url —— Ollama 默认
 * http://localhost:11434，vLLM 走 OpenAI 兼容格式 8080/v1——实际以
 * yaml 传入值为准（2026-08-20 审查：代码默认与 yaml 默认的差异为
 * 文档级，非功能缺陷）。 */
#define LOCAL_DEFAULT_BASE "http://localhost:8080/v1"
#define LOCAL_DEFAULT_MODEL "gpt-3.5-turbo"
#define LOCAL_DEFAULT_TIMEOUT 60.0

typedef struct {
    provider_base_ctx_t base;
} local_ctx_t;

static provider_ctx_t *local_init(const char *name __attribute__((unused)),
                                  const char *api_key __attribute__((unused)), const char *api_base,
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
    provider_base_init(&ctx->base, NULL, api_base, NULL, timeout, max_retries, LOCAL_DEFAULT_BASE);

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

    size_t req_body_len = strlen(req_body);
    (void)req_body_len;

    char url[1024];
    snprintf(url, sizeof(url), "%s/chat/completions", base->api_base);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    provider_http_resp_t *http_resp = NULL;
    long http_code = 0;

    SVC_LOG_DEBUG("C-L02: LOCAL: HTTP-POST url=%s body_len=%zu timeout=%.1fs retries=%d "
                  "(no auth, local endpoint)",
                  url, req_body_len, base->timeout_sec, base->max_retries);

    int ret = provider_http_post(url, headers, req_body, base->timeout_sec, base->max_retries,
                                 &http_resp, &http_code);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: LOCAL: COMPLETE-FAIL — HTTP request failed "
                      "url=%s http_code=%ld ret=%d timeout=%.1fs "
                      "STACK: provider_http_post() → local_complete()",
                      url, http_code, ret, base->timeout_sec);
        return ret;
    }

    if (http_code != 200) {
        size_t resp_body_len = http_resp ? strlen(http_resp->data) : 0;
        SVC_LOG_ERROR(
            "C-L02: LOCAL: COMPLETE-FAIL — HTTP error "
            "url=%s http_code=%ld resp_body_len=%zu "
            "DIAGNOSIS: %s "
            "STACK: provider_http_post() → local_complete()",
            url, http_code, resp_body_len,
            (http_code == 401) ? "invalid API key (but local should not use auth)" :
            (http_code == 429) ? "rate limited" :
            (http_code == 500) ? "local server internal error" :
            (http_code == 503) ? "local service unavailable — check if model server is running" :
                                 "check local endpoint URL and model server status");
        provider_http_resp_free(http_resp);
        return AIRY_ERR_IO;
    }

    size_t resp_body_len = http_resp ? strlen(http_resp->data) : 0;
    SVC_LOG_DEBUG("C-L02: LOCAL: HTTP-RESPONSE http_code=%ld resp_body_len=%zu", http_code,
                  resp_body_len);

    ret = provider_parse_openai_response(http_resp->data, out_response);
    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: LOCAL: COMPLETE-FAIL — response parse failed "
                      "ret=%d resp_body_len=%zu "
                      "STACK: provider_parse_openai_response() → local_complete()",
                      ret, resp_body_len);
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

/* Streaming tool-call accumulation: local (OpenAI-compatible) providers
 * stream tool_call deltas in the standard {index, id?} then
 * {index, function.{name?, arguments}} shape. The complete array is emitted
 * once at stream end as a control frame (see provider_emit_tool_frame). */
#define LOC_STREAM_MAX_TOOL_CALLS 16

typedef struct {
    int index;
    char id[128];
    char name[128];
    char *args;
    size_t args_len;
    size_t args_cap;
} loc_tool_acc_t;

typedef struct {
    llm_stream_callback_t user_cb;
    void *user_data;
    char *acc_content;
    size_t acc_cap;
    size_t acc_len;
    char *resp_id;
    char *resp_model;
    uint64_t resp_created;
    char *finish_reason;
    /* Streaming usage accumulation: local OpenAI-compatible endpoints
     * (Ollama/vLLM/llama.cpp) attach the usage block in the final chunk when
     * stream_options.include_usage is set; parse it so streaming token
     * stats are not always zero. */
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;
    uint32_t reasoning_tokens;
    loc_tool_acc_t tools[LOC_STREAM_MAX_TOOL_CALLS];
    size_t tool_count;
} loc_stream_acc_t;

static loc_tool_acc_t *loc_tool_find_or_add(loc_stream_acc_t *acc, int index)
{
    for (size_t k = 0; k < acc->tool_count; k++) {
        if (acc->tools[k].index == index)
            return &acc->tools[k];
    }
    if (acc->tool_count >= LOC_STREAM_MAX_TOOL_CALLS)
        return NULL;
    loc_tool_acc_t *slot = &acc->tools[acc->tool_count++];
    __builtin_memset(slot, 0, sizeof(*slot));
    slot->index = index;
    return slot;
}

static void loc_tool_append_args(loc_tool_acc_t *slot, const char *frag)
{
    size_t flen = strlen(frag);
    if (flen == 0)
        return;
    size_t need = slot->args_len + flen + 1;
    if (need > slot->args_cap) {
        size_t cap = slot->args_cap ? slot->args_cap : 256;
        while (cap < need)
            cap *= 2;
        char *grown = (char *)AIRY_REALLOC(slot->args, cap);
        if (!grown)
            return;
        slot->args = grown;
        slot->args_cap = cap;
    }
    __builtin_memcpy(slot->args + slot->args_len, frag, flen);
    slot->args_len += flen;
    slot->args[slot->args_len] = '\0';
}

static char *loc_build_tool_calls_json(const loc_stream_acc_t *acc)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return NULL;
    for (size_t i = 0; i < acc->tool_count; i++) {
        const loc_tool_acc_t *slot = &acc->tools[i];
        cJSON *tc = cJSON_CreateObject();
        cJSON *fn = cJSON_CreateObject();
        if (!tc || !fn) {
            if (tc)
                cJSON_Delete(tc);
            if (fn)
                cJSON_Delete(fn);
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddStringToObject(tc, "id", slot->id[0] ? slot->id : "call_unknown");
        cJSON_AddStringToObject(tc, "type", "function");
        if (slot->name[0])
            cJSON_AddStringToObject(fn, "name", slot->name);
        cJSON_AddStringToObject(fn, "arguments", slot->args ? slot->args : "");
        cJSON_AddItemToObject(tc, "function", fn);
        cJSON_AddItemToArray(arr, tc);
    }
    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return js;
}

static void loc_stream_tools_cleanup(loc_stream_acc_t *acc)
{
    for (size_t i = 0; i < acc->tool_count; i++)
        AIRY_FREE(acc->tools[i].args);
    acc->tool_count = 0;
}

static int loc_stream_on_chunk(const char *json_line, void *userdata)
{
    loc_stream_acc_t *acc = (loc_stream_acc_t *)userdata;

    CJSON_PARSE_GUARD(root, json_line, { return 0; });

    if (!acc->resp_id) {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsString(id) && id->valuestring)
            acc->resp_id = AIRY_STRDUP(id->valuestring);
    }

    if (!acc->resp_model) {
        cJSON *model = cJSON_GetObjectItem(root, "model");
        if (cJSON_IsString(model) && model->valuestring)
            acc->resp_model = AIRY_STRDUP(model->valuestring);
    }

    cJSON *created = cJSON_GetObjectItem(root, "created");
    if (cJSON_IsNumber(created) && acc->resp_created == 0)
        acc->resp_created = (uint64_t)created->valuedouble;

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *delta = cJSON_GetObjectItem(choice, "delta");
        if (delta) {
            cJSON *content = cJSON_GetObjectItem(delta, "content");
            if (cJSON_IsString(content) && content->valuestring) {
                const char *text = content->valuestring;
                size_t tlen = strlen(text);

                if (acc->user_cb)
                    acc->user_cb(text, acc->user_data);

                if (tlen > 0) {
                    size_t needed = acc->acc_len + tlen + 1;
                    if (needed > acc->acc_cap) {
                        size_t new_cap = acc->acc_cap * 2;
                        while (new_cap < needed)
                            new_cap *= 2;
                        char *ptr = (char *)AIRY_REALLOC(acc->acc_content, new_cap);
                        if (ptr) {
                            acc->acc_content = ptr;
                            acc->acc_cap = new_cap;
                        }
                    }
                    if (acc->acc_content && acc->acc_len + tlen < acc->acc_cap) {
                        __builtin_memcpy(acc->acc_content + acc->acc_len, text, tlen);
                        acc->acc_len += tlen;
                        acc->acc_content[acc->acc_len] = '\0';
                    }
                }
            }

            /* Tool-call deltas (OpenAI-compatible): accumulate fragments per
             * index; the complete array is emitted as a control frame at
             * stream end so streaming clients keep the tool loop. */
            cJSON *tcs = cJSON_GetObjectItem(delta, "tool_calls");
            if (cJSON_IsArray(tcs)) {
                int tn = cJSON_GetArraySize(tcs);
                for (int ti = 0; ti < tn; ti++) {
                    cJSON *tc = cJSON_GetArrayItem(tcs, ti);
                    cJSON *idxj = cJSON_GetObjectItem(tc, "index");
                    int idx = (cJSON_IsNumber(idxj)) ? (int)idxj->valuedouble :
                                                       (int)acc->tool_count;
                    loc_tool_acc_t *slot = loc_tool_find_or_add(acc, idx);
                    if (!slot)
                        continue;
                    cJSON *idj = cJSON_GetObjectItem(tc, "id");
                    if (cJSON_IsString(idj) && idj->valuestring && !slot->id[0]) {
                        size_t idlen = strlen(idj->valuestring);
                        if (idlen >= sizeof(slot->id))
                            idlen = sizeof(slot->id) - 1;
                        __builtin_memcpy(slot->id, idj->valuestring, idlen);
                        slot->id[idlen] = '\0';
                    }
                    cJSON *fn = cJSON_GetObjectItem(tc, "function");
                    if (cJSON_IsObject(fn)) {
                        cJSON *namej = cJSON_GetObjectItem(fn, "name");
                        if (cJSON_IsString(namej) && namej->valuestring && !slot->name[0]) {
                            size_t nlen = strlen(namej->valuestring);
                            if (nlen >= sizeof(slot->name))
                                nlen = sizeof(slot->name) - 1;
                            __builtin_memcpy(slot->name, namej->valuestring, nlen);
                            slot->name[nlen] = '\0';
                        }
                        cJSON *argj = cJSON_GetObjectItem(fn, "arguments");
                        if (cJSON_IsString(argj) && argj->valuestring)
                            loc_tool_append_args(slot, argj->valuestring);
                    }
                }
            }
        }

        cJSON *fr = cJSON_GetObjectItem(choice, "finish_reason");
        if (cJSON_IsString(fr) && fr->valuestring && strcmp(fr->valuestring, "null") != 0) {
            AIRY_FREE(acc->finish_reason);
            acc->finish_reason = AIRY_STRDUP(fr->valuestring);
        }
    }

    /* 流式末尾 chunk 携带 usage（include_usage 时最后 chunk 仅 usage 无
     * choices）。最后一次解析覆盖前面（usage 在流末尾最完整）。 */
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

static llm_response_t *loc_build_stream_response(loc_stream_acc_t *acc)
{
    llm_response_t *resp = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    resp->id = acc->resp_id ? acc->resp_id : AIRY_STRDUP("");
    acc->resp_id = NULL;
    resp->model = acc->resp_model ? acc->resp_model : AIRY_STRDUP("unknown");
    acc->resp_model = NULL;
    resp->created = acc->resp_created;
    resp->choices = (llm_message_t *)AIRY_CALLOC(1, sizeof(llm_message_t));
    if (resp->choices) {
        resp->choice_count = 1;
        resp->choices[0].role = AIRY_STRDUP("assistant");
        resp->choices[0].content = acc->acc_content;
        acc->acc_content = NULL;
    } else {
        resp->choice_count = 0;
    }
    resp->finish_reason = acc->finish_reason ? acc->finish_reason : AIRY_STRDUP("stop");
    acc->finish_reason = NULL;
    resp->prompt_tokens = acc->prompt_tokens;
    resp->completion_tokens = acc->completion_tokens;
    resp->total_tokens = acc->total_tokens;
    resp->reasoning_tokens = acc->reasoning_tokens;
    return resp;
}

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

    char url[1024];
    snprintf(url, sizeof(url), "%s/chat/completions", base->api_base);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    loc_stream_acc_t acc;
    __builtin_memset(&acc, 0, sizeof(acc));
    acc.user_cb = callback;
    acc.user_data = user_data;
    acc.acc_cap = 4096;
    acc.acc_content = (char *)AIRY_MALLOC(acc.acc_cap);

    SVC_LOG_DEBUG("C-L02: LOCAL: STREAM-HTTP-POST url=%s body_len=%zu timeout=%.1fs "
                  "(no auth, local endpoint)",
                  url, strlen(req_body), base->timeout_sec);

    long http_code = 0;
    int ret = provider_http_post_stream(url, headers, req_body, base->timeout_sec,
                                        loc_stream_on_chunk, &acc, &http_code);

    curl_slist_free_all(headers);
    AIRY_FREE(req_body);

    if (ret != AIRY_OK) {
        SVC_LOG_ERROR("C-L02: LOCAL: STREAM-FAIL — HTTP stream error "
                      "url=%s http_code=%ld ret=%d timeout=%.1fs "
                      "STACK: provider_http_post_stream() → local_complete_stream()",
                      url, http_code, ret, base->timeout_sec);
        AIRY_FREE(acc.acc_content);
        AIRY_FREE(acc.resp_id);
        AIRY_FREE(acc.resp_model);
        AIRY_FREE(acc.finish_reason);
        loc_stream_tools_cleanup(&acc);
        return ret;
    }

    llm_response_t *resp = loc_build_stream_response(&acc);
    AIRY_FREE(acc.acc_content);
    AIRY_FREE(acc.resp_id);
    AIRY_FREE(acc.resp_model);
    AIRY_FREE(acc.finish_reason);

    /* Streaming tool calls: emit the accumulated array as a control frame
     * before the stream closes so IPC clients (CLI tool loop) see the same
     * tool_calls a non-streaming round would carry. */
    if (acc.tool_count > 0) {
        char *tc_json = loc_build_tool_calls_json(&acc);
        if (tc_json) {
            provider_emit_tool_frame(callback, user_data, tc_json);
            if (resp && resp->choices && resp->choice_count > 0 && !resp->choices[0].tool_calls_json)
                resp->choices[0].tool_calls_json = tc_json;
            else
                AIRY_FREE(tc_json);
        }
    }
    loc_stream_tools_cleanup(&acc);

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
        llm_response_free(resp);

    return AIRY_OK;
}

const provider_ops_t local_ops = {.init = local_init,
                                  .destroy = local_destroy,
                                  .complete = local_complete,
                                  .complete_stream = local_complete_stream,
                                  .name = "local",
                                  .default_model = LOCAL_DEFAULT_MODEL,
                                  .default_base_url = LOCAL_DEFAULT_BASE};
