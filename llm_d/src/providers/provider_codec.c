// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file provider_codec.c
 * @brief OpenAI 兼容协议编解码（请求组装 / 响应解析）。
 *
 * 域拆分自 provider.c（2026-08-27）：provider_build_openai_request /
 * provider_parse_openai_response，供 openai / deepseek / local 等
 * OpenAI 兼容 provider 共用。
 */

#include "airy_memory.h"
#include "error.h"
#include "provider.h"
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

    const char *model = manager->model && manager->model[0] ? manager->model : default_model;
    cJSON_AddStringToObject(root, "model", model ? model : "gpt-3.5-turbo");
    cJSON_AddNumberToObject(root, "temperature",
                            manager->temperature > 0 ? manager->temperature : 0.7);

    if (manager->top_p > 0) {
        cJSON_AddNumberToObject(root, "top_p", manager->top_p);
    }

    if (manager->max_tokens > 0) {
        cJSON_AddNumberToObject(root, "max_tokens", manager->max_tokens);
    }

    if (manager->stream) {
        cJSON_AddBoolToObject(root, "stream", 1);
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

    if (manager->stop_count > 0 && manager->stop) {
        cJSON *stop = cJSON_CreateArray();
        for (size_t i = 0; i < manager->stop_count; ++i) {
            cJSON_AddItemToArray(stop, cJSON_CreateString(manager->stop[i]));
        }
        cJSON_AddItemToObject(root, "stop", stop);
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

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id) && id->valuestring) {
        resp->id = AIRY_STRDUP(id->valuestring);
    }

    cJSON *model = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(model) && model->valuestring) {
        resp->model = AIRY_STRDUP(model->valuestring);
    }

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

            llm_response_free(resp);
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
                resp->finish_reason = AIRY_STRDUP(finish->valuestring);
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
