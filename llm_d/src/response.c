// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#include "airy_memory.h"
#include "error.h"
/**
 * @file response.c
 * @brief 响应序列化实现
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "response.h"

/* P0.18.2: 引入 cjson_helpers.h 提供 CJSON_PARSE_GUARD/CJSON_AUTO_FREE 宏
 * （response.h 已传递 <cjson/cJSON.h>，cjson_helpers.h 依赖 AIRY_HAS_CJSON） */
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
    /* 成本（USD）：网关/OpenAI 兼容适配器据此累计金额 */
    cJSON_AddNumberToObject(root, "cost_usd", resp->cost_usd);
    /* usage 嵌套对象：OpenAI chat.completions 兼容格式，网关 parse_llm_result
     * 与 OpenAI 适配器均读取此节点；顶层 prompt/completion/total_tokens 保留兼容 */
    cJSON *usage = cJSON_CreateObject();
    cJSON_AddNumberToObject(usage, "prompt_tokens", resp->prompt_tokens);
    cJSON_AddNumberToObject(usage, "completion_tokens", resp->completion_tokens);
    cJSON_AddNumberToObject(usage, "total_tokens", resp->total_tokens);
    cJSON_AddItemToObject(root, "usage", usage);
    if (resp->finish_reason)
        cJSON_AddStringToObject(root, "finish_reason", resp->finish_reason);

    cJSON *choices = cJSON_CreateArray();
    for (size_t i = 0; i < resp->choice_count; ++i) {
        cJSON *choice = cJSON_CreateObject();
        cJSON_AddStringToObject(choice, "role", resp->choices[i].role);
        /* LLM 工具调用轮 content 常为 NULL（响应仅含 tool_calls），
         * cJSON_AddStringToObject 对 NULL 行为不确定，显式回退空字符串
         * 保证 choices[i].content 字段恒为合法字符串（网关可解析） */
        cJSON_AddStringToObject(choice, "content",
                                resp->choices[i].content ? resp->choices[i].content : "");
        /* Function calling：透传 assistant 的 tool_calls（原始 JSON 数组） */
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
    /* P0.18.2: CJSON_PARSE_GUARD 替代 cJSON_Parse + NULL 检查 + 手动 cJSON_Delete */
    CJSON_PARSE_GUARD(root, json, {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    });

    llm_response_t *resp = AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp) {
        /* root 由 CJSON_AUTO_FREE 自动释放，无需手动 cJSON_Delete */
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

    /* usage 嵌套对象（response_to_json 新格式）：优先读取，顶层字段保留兼容 */
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
            /* root 由 CJSON_AUTO_FREE 自动释放 */
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
            /* Function calling：与 response_to_json 对称，回读 assistant 的 tool_calls */
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

    /* root 由 CJSON_AUTO_FREE 自动释放，无需手动 cJSON_Delete(root) */
    return resp;
}