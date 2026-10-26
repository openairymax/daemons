// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file envelope.c
 * @brief 请求/响应信封编解码（B16-S3 适配收敛的共享件 SSoT）。
 *
 * 域拆分自 provider.c（2026-08-27）：prov_build_chat /
 * prov_parse_chat 供 chat 线格式各适配层共用。B16-S3.3 追加
 * 各映射表的公共段下沉件：参数段骨架（stop 键名表驱动）、响应身份段、
 * tools / tool_calls 遍历原语——适配层据自身线格式改写对应段，
 * 不再各自重写同构段。
 */

#include "airy_memory.h"
#include "error.h"
#include "provider_envelope.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <cjson_helpers.h>
#include <stdlib.h>
#include <string.h>

char *prov_build_chat(const llm_request_config_t *manager, const char *default_model,
                              const char *extra_json)
{
    if (!manager) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        SVC_LOG_ERROR("C-L02: PROVIDER: REQUEST-BUILD-FAIL reason=oom_root "
                      "STACK: prov_build_chat");
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    /* 参数段骨架（B16-S3.3 下沉件）：model / temperature / max_tokens /
     * top_p / stream / stop 六项，stop 键名由调用方按线格式给出；
     * max_tokens 兜底传 0 = 调用方未给则不写（可缺省语义）。 */
    provider_request_params_fill(root, manager, default_model, 0, "stop");

    if (manager->stream) {
        /* 流式请求必须显式声明 include_usage：chat 兼容端点
         * （vLLM/llama.cpp 等）默认不回传 usage，不声明则 token 计数与
         * 计费恒为 0。已回传的端点显式声明后幂等。 */
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

        /* 推理型端点要求把 assistant 轮的 reasoning_content 在重发时
         * 原样回传；工具多轮之间丢弃该字段会招致上游 HTTP 400。 */
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

    /* 通用供应商扩展段：逐键并入顶层，键冲突时先删旧键再挂新键
     * （扩展段为准）。空串 / 非法 JSON 静默跳过，不阻断请求装配。 */
    if (extra_json && extra_json[0]) {
        CJSON_PARSE_GUARD(extra, extra_json, {});
        if (cJSON_IsObject(extra)) {
            const cJSON *field = NULL;
            cJSON_ArrayForEach(field, extra)
            {
                if (field->string) {
                    cJSON_DeleteItemFromObject(root, field->string);
                    cJSON_AddItemToObject(root, field->string, cJSON_Duplicate(field, 1));
                }
            }
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

int prov_parse_chat(const char *body, llm_response_t **out)
{
    if (!body || !out) {
        return AIRY_ERR_INVALID_PARAM;
    }

    CJSON_PARSE_GUARD(root, body, {
        SVC_LOG_ERROR("C-L02: PROVIDER: PARSE-FAIL reason=cjson_parse_error "
                      "STACK: prov_parse_chat");
        return AIRY_ERR_PARSE_ERROR;
    });

    llm_response_t *resp = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp) {
        SVC_LOG_ERROR("C-L02: PROVIDER: PARSE-FAIL reason=oom_resp "
                      "STACK: prov_parse_chat");

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
                          "STACK: prov_parse_chat");

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
                resp->finish_reason = AIRY_STRDUP(llm_finish_reason_norm(finish->valuestring));
            }
        }
    }

    provider_usage_load(cJSON_GetObjectItem(root, "usage"), &resp->prompt_tokens,
                        &resp->completion_tokens, &resp->total_tokens,
                        &resp->reasoning_tokens, &resp->cached_tokens);
    provider_timings_load(cJSON_GetObjectItem(root, "timings"), &resp->timings);

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

void prov_tools_each(const char *tools_json, provider_tool_fn fn, void *ud)
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

void prov_tool_each(const char *tool_calls_json, provider_tool_call_fn fn, void *ud)
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
