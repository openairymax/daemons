// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file response.c
 * @brief Response serialization implementation.
 */

#include "response.h"

/* B16-S2：本文件属 rpc 域（src 顶层），消费发布 API llm_response_free 合法；
 * providers/ 与 router/ 内层则一律走 provider.h 的域内析构器。 */
#include "llm_service.h"

/* CJSON_PARSE_GUARD / CJSON_AUTO_FREE 来自 cjson_helpers.h（response.h
 * 已引入 <cjson/cJSON.h>，本头依赖 AIRY_HAS_CJSON）。 */
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

/* 取值器把「找键 → 类型校验 → 赋值」收敛成一行调用；不属于目标类型的字段
 * 保持原值（调用点字段初值为 NULL），与逐个 GetObjectItem 的旧行为一致。 */
static char *json_dup_str(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? AIRY_STRDUP(v->valuestring) : NULL;
}

static void json_take_u32(cJSON *obj, const char *key, uint32_t *dst)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(v))
        *dst = (uint32_t)v->valuedouble;
}

static void json_take_u64(cJSON *obj, const char *key, uint64_t *dst)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(v))
        *dst = (uint64_t)v->valuedouble;
}

static void json_take_f64(cJSON *obj, const char *key, double *dst)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(v))
        *dst = v->valuedouble;
}

/* 嵌套 usage 块覆盖顶层同名计数：部分端点两处都发，嵌套的更准确。 */
static void resp_take_usage(cJSON *root, llm_response_t *resp)
{
    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (!cJSON_IsObject(usage))
        return;
    json_take_u32(usage, "prompt_tokens", &resp->prompt_tokens);
    json_take_u32(usage, "completion_tokens", &resp->completion_tokens);
    json_take_u32(usage, "total_tokens", &resp->total_tokens);
    json_take_u32(usage, "reasoning_tokens", &resp->reasoning_tokens);
}

static int resp_take_choice(cJSON *choice, llm_message_t *out)
{
    out->role = json_dup_str(choice, "role");
    out->content = json_dup_str(choice, "content");
    out->reasoning_content = json_dup_str(choice, "reasoning_content");
    cJSON *tc = cJSON_GetObjectItem(choice, "tool_calls");
    if (!cJSON_IsArray(tc) || cJSON_GetArraySize(tc) == 0)
        return 0;
    out->tool_calls_json = cJSON_PrintUnformatted(tc);
    return out->tool_calls_json ? 0 : -1;
}

static int resp_take_choices(cJSON *root, llm_response_t *resp)
{
    cJSON *arr = cJSON_GetObjectItem(root, "choices");
    if (!cJSON_IsArray(arr))
        return 0;
    resp->choice_count = cJSON_GetArraySize(arr);
    resp->choices = AIRY_CALLOC(resp->choice_count, sizeof(llm_message_t));
    if (!resp->choices)
        return -1;
    for (size_t i = 0; i < resp->choice_count; ++i) {
        if (resp_take_choice(cJSON_GetArrayItem(arr, i), &resp->choices[i]) != 0)
            return -1;
    }
    return 0;
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

    resp->id = json_dup_str(root, "id");
    resp->model = json_dup_str(root, "model");
    json_take_u64(root, "created", &resp->created);
    json_take_u32(root, "prompt_tokens", &resp->prompt_tokens);
    json_take_u32(root, "completion_tokens", &resp->completion_tokens);
    json_take_u32(root, "total_tokens", &resp->total_tokens);
    resp_take_usage(root, resp);

    /* 兼容顶层 reasoning_tokens 字段（部分端点直接输出顶层而非嵌套） */
    if (resp->reasoning_tokens == 0)
        json_take_u32(root, "reasoning_tokens", &resp->reasoning_tokens);

    json_take_f64(root, "cost_usd", &resp->cost_usd);
    resp->finish_reason = json_dup_str(root, "finish_reason");

    if (resp_take_choices(root, resp) != 0) {
        llm_response_free(resp);
        return NULL;
    }

    return resp;
}
