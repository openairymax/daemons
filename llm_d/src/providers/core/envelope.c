// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file envelope.c
 * @brief 请求/响应信封编解码（B16-S3 适配收敛的共享件 SSoT）。
 *
 * 域拆分自 provider.c（2026-08-27）：provider_build_openai_request /
 * provider_parse_openai_response 供 openai / deepseek / local 等
 * OpenAI 兼容 provider 共用。B16-S3.3 追加三家映射表的公共段下沉件：
 * 参数段骨架（stop 键名表驱动）、响应身份段、OpenAI 形状 tools /
 * tool_calls 遍历原语——适配层（含 anthropic）据此改写本家形状，
 * 不再各自重写同构段。
 */

#include "airy_memory.h"
#include "error.h"
#include "transport.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdlib.h>
#include <string.h>

char *provider_build_openai_request(const llm_request_config_t *manager, const char *default_model)
{
    if (!manager) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        SVC_LOG_ERROR("C-L02: PROVIDER: REQUEST-BUILD-FAIL reason=oom_root "
                      "STACK: provider_build_openai_request");
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    /* 参数段骨架（B16-S3.3 下沉件）：model / temperature / max_tokens /
     * top_p / stream / stop 六项，stop 键名 "stop" 为 OpenAI 形状；
     * max_tokens 兜底传 0 = 调用方未给则不写（OpenAI 语义：可缺省）。 */
    provider_request_params_fill(root, manager, default_model, 0, "stop");

    if (manager->stream) {
        /* 2.1.1.5 修复：流式请求必须显式声明 include_usage——OpenAI 及其
         * 兼容端点（vLLM/llama.cpp 等）默认在流式 chunk 中不回传 usage，
         * 不声明则 prompt/completion/total_tokens 恒为 0，真实 token 消耗
         * 与计费无法体现。DeepSeek 默认在末尾 chunk 附带 usage，显式声明
         * 后同样生效（幂等）。 */
        cJSON *stream_options = cJSON_CreateObject();
        if (stream_options) {
            cJSON_AddBoolToObject(stream_options, "include_usage", 1);
            cJSON_AddItemToObject(root, "stream_options", stream_options);
        }
    }

    if (manager->presence_penalty != 0) {
        cJSON_AddNumberToObject(root, "presence_penalty", manager->presence_penalty);
    }

    if (manager->frequency_penalty != 0) {
        cJSON_AddNumberToObject(root, "frequency_penalty", manager->frequency_penalty);
    }

    if (manager->tools_json && manager->tools_json[0]) {
        CJSON_PARSE_GUARD(tools, manager->tools_json, {});
        if (cJSON_IsArray(tools) && cJSON_GetArraySize(tools) > 0) {
            cJSON_AddItemToObject(root, "tools", cJSON_Duplicate(tools, 1));
        }
    }

    cJSON *msgs = cJSON_CreateArray();
    for (size_t i = 0; i < manager->message_count; ++i) {
        cJSON *msg = cJSON_CreateObject();
        const char *role = manager->messages[i].role ? manager->messages[i].role : "user";
        const char *content = manager->messages[i].content ? manager->messages[i].content : "";
        cJSON_AddStringToObject(msg, "role", role);
        cJSON_AddStringToObject(msg, "content", content);

        /* Reasoning models (DeepSeek/Kimi) require the assistant turn's
         * reasoning_content to be echoed back on re-send; dropping it
         * between tool-loop turns yields HTTP 400 from the upstream API. */
        if (manager->messages[i].reasoning_content &&
            manager->messages[i].reasoning_content[0]) {
            cJSON_AddStringToObject(msg, "reasoning_content",
                                    manager->messages[i].reasoning_content);
        }

        if (manager->messages[i].tool_call_id && manager->messages[i].tool_call_id[0]) {
            cJSON_AddStringToObject(msg, "tool_call_id", manager->messages[i].tool_call_id);
        }

        if (manager->messages[i].tool_calls_json && manager->messages[i].tool_calls_json[0]) {
            CJSON_PARSE_GUARD(tc, manager->messages[i].tool_calls_json, {});
            if (cJSON_IsArray(tc)) {
                cJSON_AddItemToObject(msg, "tool_calls", cJSON_Duplicate(tc, 1));
            }
        }
        cJSON_AddItemToArray(msgs, msg);
    }
    cJSON_AddItemToObject(root, "messages", msgs);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

int provider_parse_openai_response(const char *body, llm_response_t **out)
{
    if (!body || !out) {
        return AIRY_ERR_INVALID_PARAM;
    }

    CJSON_PARSE_GUARD(root, body, {
        SVC_LOG_ERROR("C-L02: PROVIDER: PARSE-FAIL reason=cjson_parse_error "
                      "STACK: provider_parse_openai_response");
        return AIRY_ERR_PARSE_ERROR;
    });

    llm_response_t *resp = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp) {
        SVC_LOG_ERROR("C-L02: PROVIDER: PARSE-FAIL reason=oom_resp "
                      "STACK: provider_parse_openai_response");

        return AIRY_ERR_OUT_OF_MEMORY;
    }

    /* 身份段（B16-S3.3 下沉件）：id / model 两家响应同名同义。 */
    provider_response_identity_load(resp, root);

    cJSON *created = cJSON_GetObjectItem(root, "created");
    if (cJSON_IsNumber(created)) {
        resp->created = (uint64_t)created->valuedouble;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices)) {
        int size = cJSON_GetArraySize(choices);
        resp->choice_count = (size_t)size;
        resp->choices = (llm_message_t *)AIRY_CALLOC((size_t)size, sizeof(llm_message_t));

        if (!resp->choices) {
            SVC_LOG_ERROR("C-L02: PROVIDER: PARSE-FAIL reason=oom_choices "
                          "STACK: provider_parse_openai_response");

            provider_response_free(resp);
            return AIRY_ERR_OUT_OF_MEMORY;
        }

        for (int i = 0; i < size; ++i) {
            cJSON *choice = cJSON_GetArrayItem(choices, i);
            cJSON *message = cJSON_GetObjectItem(choice, "message");
            if (message) {
                cJSON *role = cJSON_GetObjectItem(message, "role");
                cJSON *content = cJSON_GetObjectItem(message, "content");
                if (cJSON_IsString(role) && role->valuestring) {
                    resp->choices[i].role = AIRY_STRDUP(role->valuestring);
                }
                if (cJSON_IsString(content) && content->valuestring) {
                    resp->choices[i].content = AIRY_STRDUP(content->valuestring);
                }

                cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning_content");
                if (cJSON_IsString(reasoning) && reasoning->valuestring) {
                    resp->choices[i].reasoning_content = AIRY_STRDUP(reasoning->valuestring);
                }

                cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
                if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
                    char *tc_json = cJSON_PrintUnformatted(tool_calls);
                    if (tc_json) {
                        resp->choices[i].tool_calls_json = tc_json;
                    }
                }
            }
            cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
            if (cJSON_IsString(finish) && finish->valuestring && !resp->finish_reason) {
                resp->finish_reason = AIRY_STRDUP(llm_finish_reason_norm(finish->valuestring));
            }
        }
    }

    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (usage) {
        cJSON *prompt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON *completion = cJSON_GetObjectItem(usage, "completion_tokens");
        cJSON *total = cJSON_GetObjectItem(usage, "total_tokens");
        if (cJSON_IsNumber(prompt))
            resp->prompt_tokens = (uint32_t)prompt->valuedouble;
        if (cJSON_IsNumber(completion))
            resp->completion_tokens = (uint32_t)completion->valuedouble;
        if (cJSON_IsNumber(total))
            resp->total_tokens = (uint32_t)total->valuedouble;
        /* Thinking tokens: either top-level usage.reasoning_tokens (some
         * endpoints) or nested completion_tokens_details.reasoning_tokens
         * (DeepSeek/OpenAI). Parse both so the count survives everywhere. */
        cJSON *rt = cJSON_GetObjectItem(usage, "reasoning_tokens");
        if (!cJSON_IsNumber(rt)) {
            cJSON *details = cJSON_GetObjectItem(usage, "completion_tokens_details");
            if (cJSON_IsObject(details))
                rt = cJSON_GetObjectItem(details, "reasoning_tokens");
        }
        if (cJSON_IsNumber(rt))
            resp->reasoning_tokens = (uint32_t)rt->valuedouble;
    }

    *out = resp;
    return AIRY_OK;
}

void provider_request_params_fill(cJSON *root, const llm_request_config_t *manager,
                                  const char *default_model, int default_max_tokens,
                                  const char *stop_key)
{
    if (!root || !manager || !stop_key)
        return;

    const char *model = (manager->model && manager->model[0]) ? manager->model : default_model;
    cJSON_AddStringToObject(root, "model", model ? model : "");
    cJSON_AddNumberToObject(root, "temperature",
                            manager->temperature > 0 ? manager->temperature : 0.7);

    int max_tokens = manager->max_tokens > 0 ? manager->max_tokens : default_max_tokens;
    if (max_tokens > 0)
        cJSON_AddNumberToObject(root, "max_tokens", max_tokens);

    if (manager->top_p > 0)
        cJSON_AddNumberToObject(root, "top_p", manager->top_p);

    if (manager->stream)
        cJSON_AddBoolToObject(root, "stream", 1);

    if (manager->stop_count > 0 && manager->stop) {
        cJSON *stop = cJSON_CreateArray();
        for (size_t i = 0; i < manager->stop_count; ++i)
            cJSON_AddItemToArray(stop, cJSON_CreateString(manager->stop[i]));
        cJSON_AddItemToObject(root, stop_key, stop);
    }
}

void provider_response_identity_load(llm_response_t *resp, const cJSON *root)
{
    if (!resp || !root)
        return;

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id) && id->valuestring)
        resp->id = AIRY_STRDUP(id->valuestring);

    cJSON *model = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(model) && model->valuestring)
        resp->model = AIRY_STRDUP(model->valuestring);
}

void provider_openai_tools_foreach(const char *tools_json, provider_openai_tool_fn fn, void *ud)
{
    if (!tools_json || !tools_json[0] || !fn)
        return;

    CJSON_PARSE_GUARD(src, tools_json, { return; });
    if (!cJSON_IsArray(src))
        return;

    int count = cJSON_GetArraySize(src);
    for (int i = 0; i < count; ++i) {
        cJSON *func = cJSON_GetObjectItem(cJSON_GetArrayItem(src, i), "function");
        cJSON *name = cJSON_GetObjectItem(func, "name");
        if (!cJSON_IsString(name) || !name->valuestring)
            continue;
        cJSON *desc = cJSON_GetObjectItem(func, "description");
        fn(ud, name->valuestring,
           (cJSON_IsString(desc) && desc->valuestring) ? desc->valuestring : NULL,
           cJSON_GetObjectItem(func, "parameters"));
    }
}

void provider_openai_tool_calls_foreach(const char *tool_calls_json,
                                        provider_openai_tool_call_fn fn, void *ud)
{
    if (!tool_calls_json || !tool_calls_json[0] || !fn)
        return;

    CJSON_PARSE_GUARD(calls, tool_calls_json, { return; });
    if (!cJSON_IsArray(calls))
        return;

    int count = cJSON_GetArraySize(calls);
    for (int i = 0; i < count; ++i) {
        cJSON *call = cJSON_GetArrayItem(calls, i);
        cJSON *func = cJSON_GetObjectItem(call, "function");
        cJSON *id = cJSON_GetObjectItem(call, "id");
        cJSON *name = cJSON_GetObjectItem(func, "name");
        cJSON *args = cJSON_GetObjectItem(func, "arguments");
        fn(ud, (cJSON_IsString(id) && id->valuestring) ? id->valuestring : "",
           (cJSON_IsString(name) && name->valuestring) ? name->valuestring : "",
           (cJSON_IsString(args) && args->valuestring) ? args->valuestring : NULL);
    }
}
