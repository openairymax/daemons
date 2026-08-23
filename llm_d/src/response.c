// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file response.c
 * @brief Response serialization implementation.
 */

#include "response.h"

/* P0.18.2: cjson_helpers.h provides the CJSON_PARSE_GUARD/CJSON_AUTO_FREE
 * macros (response.h already pulls in <cjson/cJSON.h>; cjson_helpers.h
 * depends on AIRY_HAS_CJSON). */
#include <cjson_helpers.h>

#include <stdlib.h>
#include <string.h>

char *response_to_json(const llm_response_t *resp)
{
    if (!resp) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    if (resp->id)
        cJSON_AddStringToObject(root, "id", resp->id);
    if (resp->model)
        cJSON_AddStringToObject(root, "model", resp->model);
    cJSON_AddNumberToObject(root, "created", (double)resp->created);
    cJSON_AddNumberToObject(root, "prompt_tokens", resp->prompt_tokens);
    cJSON_AddNumberToObject(root, "completion_tokens", resp->completion_tokens);
    cJSON_AddNumberToObject(root, "total_tokens", resp->total_tokens);
    cJSON_AddNumberToObject(root, "reasoning_tokens", resp->reasoning_tokens);

    cJSON_AddNumberToObject(root, "cost_usd", resp->cost_usd);
    /* usage nested object: OpenAI chat.completions-compatible format; both
     * the gateway parse_llm_result and the OpenAI adapter read this node;
     * top-level prompt/completion/total_tokens kept for compatibility */
    cJSON *usage = cJSON_CreateObject();
    cJSON_AddNumberToObject(usage, "prompt_tokens", resp->prompt_tokens);
    cJSON_AddNumberToObject(usage, "completion_tokens", resp->completion_tokens);
    cJSON_AddNumberToObject(usage, "total_tokens", resp->total_tokens);
    cJSON_AddNumberToObject(usage, "reasoning_tokens", resp->reasoning_tokens);
    cJSON_AddItemToObject(root, "usage", usage);
    if (resp->finish_reason)
        cJSON_AddStringToObject(root, "finish_reason", resp->finish_reason);

    cJSON *choices = cJSON_CreateArray();
    for (size_t i = 0; i < resp->choice_count; ++i) {
        cJSON *choice = cJSON_CreateObject();
        cJSON_AddStringToObject(choice, "role", resp->choices[i].role);
        /* In LLM tool-calling turns content is often NULL (response only
         * carries tool_calls); cJSON_AddStringToObject's behavior with NULL
         * is unspecified, so explicitly fall back to an empty string to
         * keep choices[i].content always a valid string (gateway-parseable) */
        cJSON_AddStringToObject(choice, "content",
                                resp->choices[i].content ? resp->choices[i].content : "");

        if (resp->choices[i].reasoning_content && resp->choices[i].reasoning_content[0]) {
            cJSON_AddStringToObject(choice, "reasoning_content",
                                    resp->choices[i].reasoning_content);
        }

        if (resp->choices[i].tool_calls_json && resp->choices[i].tool_calls_json[0]) {
            CJSON_PARSE_GUARD(tc, resp->choices[i].tool_calls_json, {});
            if (cJSON_IsArray(tc)) {
                cJSON_AddItemToObject(choice, "tool_calls", cJSON_Duplicate(tc, 1));
            }
        }
        cJSON_AddItemToArray(choices, choice);
    }
    cJSON_AddItemToObject(root, "choices", choices);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

llm_response_t *response_from_json(const char *json)
{
    if (!json) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    CJSON_PARSE_GUARD(root, json, { AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed"); });

    llm_response_t *resp = AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp) {

        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id))
        resp->id = AIRY_STRDUP(id->valuestring);

    cJSON *model = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(model))
        resp->model = AIRY_STRDUP(model->valuestring);

    cJSON *created = cJSON_GetObjectItem(root, "created");
    if (cJSON_IsNumber(created))
        resp->created = (uint64_t)created->valuedouble;

    cJSON *prompt_tokens = cJSON_GetObjectItem(root, "prompt_tokens");
    if (cJSON_IsNumber(prompt_tokens))
        resp->prompt_tokens = (uint32_t)prompt_tokens->valuedouble;

    cJSON *completion_tokens = cJSON_GetObjectItem(root, "completion_tokens");
    if (cJSON_IsNumber(completion_tokens))
        resp->completion_tokens = (uint32_t)completion_tokens->valuedouble;

    cJSON *total_tokens = cJSON_GetObjectItem(root, "total_tokens");
    if (cJSON_IsNumber(total_tokens))
        resp->total_tokens = (uint32_t)total_tokens->valuedouble;

    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON *u_pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        if (cJSON_IsNumber(u_pt))
            resp->prompt_tokens = (uint32_t)u_pt->valuedouble;
        cJSON *u_ct = cJSON_GetObjectItem(usage, "completion_tokens");
        if (cJSON_IsNumber(u_ct))
            resp->completion_tokens = (uint32_t)u_ct->valuedouble;
        cJSON *u_tt = cJSON_GetObjectItem(usage, "total_tokens");
        if (cJSON_IsNumber(u_tt))
            resp->total_tokens = (uint32_t)u_tt->valuedouble;
        cJSON *u_rt = cJSON_GetObjectItem(usage, "reasoning_tokens");
        if (cJSON_IsNumber(u_rt))
            resp->reasoning_tokens = (uint32_t)u_rt->valuedouble;
    }

    /* 兼容顶层 reasoning_tokens 字段（部分端点直接输出顶层而非嵌套） */
    if (resp->reasoning_tokens == 0) {
        cJSON *t_rt = cJSON_GetObjectItem(root, "reasoning_tokens");
        if (cJSON_IsNumber(t_rt))
            resp->reasoning_tokens = (uint32_t)t_rt->valuedouble;
    }

    cJSON *cost = cJSON_GetObjectItem(root, "cost_usd");
    if (cJSON_IsNumber(cost))
        resp->cost_usd = cost->valuedouble;

    cJSON *finish_reason = cJSON_GetObjectItem(root, "finish_reason");
    if (cJSON_IsString(finish_reason))
        resp->finish_reason = AIRY_STRDUP(finish_reason->valuestring);

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices)) {
        resp->choice_count = cJSON_GetArraySize(choices);
        resp->choices = AIRY_CALLOC(resp->choice_count, sizeof(llm_message_t));
        if (!resp->choices) {

            llm_response_free(resp);
            return NULL;
        }
        for (size_t i = 0; i < resp->choice_count; ++i) {
            cJSON *choice = cJSON_GetArrayItem(choices, i);
            cJSON *role = cJSON_GetObjectItem(choice, "role");
            cJSON *content = cJSON_GetObjectItem(choice, "content");
            if (cJSON_IsString(role))
                resp->choices[i].role = AIRY_STRDUP(role->valuestring);
            if (cJSON_IsString(content))
                resp->choices[i].content = AIRY_STRDUP(content->valuestring);

            cJSON *reasoning = cJSON_GetObjectItem(choice, "reasoning_content");
            if (cJSON_IsString(reasoning))
                resp->choices[i].reasoning_content = AIRY_STRDUP(reasoning->valuestring);

            cJSON *tool_calls = cJSON_GetObjectItem(choice, "tool_calls");
            if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
                resp->choices[i].tool_calls_json = cJSON_PrintUnformatted(tool_calls);
                if (!resp->choices[i].tool_calls_json) {
                    llm_response_free(resp);
                    return NULL;
                }
            }
        }
    }

    return resp;
}