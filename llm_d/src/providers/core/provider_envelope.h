/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_envelope.h: Provider 域请求/响应共享件（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_ENVELOPE_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_ENVELOPE_H

#include "llm_service_types.h"
#include "airy_memory.h"
#include <cjson/cJSON.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

char *provider_build_openai_request(const llm_request_config_t *manager, const char *default_model);
int provider_parse_openai_response(const char *body, llm_response_t **out);
/* 请求参数段骨架：model / temperature / max_tokens / top_p / stream / stop 数组。 */
void provider_request_params_fill(cJSON *root, const llm_request_config_t *manager,
                                  const char *default_model, int default_max_tokens,
                                  const char *stop_key);

/* 响应身份段装载：id / model 两个字符串字段（两家同名同义）。 */
void provider_response_identity_load(llm_response_t *resp, const cJSON *root);

/* OpenAI 形状 tools 声明遍历：逐条提取 function 三元组（name 缺失跳过）。 */
typedef void (*provider_openai_tool_fn)(void *ud, const char *name, const char *description,
                                        const cJSON *parameters);
void provider_openai_tools_foreach(const char *tools_json, provider_openai_tool_fn fn, void *ud);

/* OpenAI 形状 assistant 轮 tool_calls 遍历：arguments 为 JSON 串原样交付。 */
typedef void (*provider_openai_tool_call_fn)(void *ud, const char *id, const char *name,
                                             const char *arguments);
void provider_openai_tool_calls_foreach(const char *tool_calls_json,
                                        provider_openai_tool_call_fn fn, void *ud);

static inline const char *provider_json_str_val(const cJSON *obj, const char *key)
{
    cJSON *v = obj ? cJSON_GetObjectItem(obj, key) : NULL;
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

static inline void provider_json_str_set(const cJSON *obj, const char *key, char **dst)
{
    const char *v = provider_json_str_val(obj, key);
    if (v) {
        AIRY_FREE(*dst);
        *dst = AIRY_STRDUP(v);
    }
}

static inline uint32_t provider_json_u32_get(const cJSON *obj, const char *key)
{
    cJSON *v = obj ? cJSON_GetObjectItem(obj, key) : NULL;
    return cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_ENVELOPE_H */
