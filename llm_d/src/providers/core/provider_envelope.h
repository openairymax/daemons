/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_envelope.h: Provider 域请求/响应共享件（SSoT，B16-S3）
 *
 * 只实现线格式本身，不持有任何厂商名、端点或默认模型：厂商差异一律由
 * model.yaml（数据面）注入，机制核仅按格式分发。 */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_ENVELOPE_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_ENVELOPE_H

#include "llm_service_types.h"
#include "airy_memory.h"
#include <cjson/cJSON.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* extra_json：通用扩展段（JSON object 片段），逐键并入请求体顶层；
 * NULL / 空串 = 无扩展。端点专有参数（如 keep_alive）经此注入，
 * 不污染 Chat Completions 通用骨架。键冲突时以扩展段为准。 */
char *prov_build_chat(const llm_request_config_t *manager, const char *default_model,
                              const char *extra_json);
int prov_parse_chat(const char *body, llm_response_t **out);
/* 请求参数段骨架：model / temperature / max_tokens / top_p / stream / stop 数组。 */
void provider_request_params_fill(cJSON *root, const llm_request_config_t *manager,
                                  const char *default_model, int default_max_tokens,
                                  const char *stop_key);

/* 响应身份段装载：id / model 两个字符串字段（各线格式同名同义）。 */
void provider_response_identity_load(llm_response_t *resp, const cJSON *root);

/* 工具声明遍历：逐条提取 function 三元组（name 缺失跳过）。 */
typedef void (*provider_tool_fn)(void *ud, const char *name, const char *description,
                                 const cJSON *parameters);
void prov_tools_each(const char *tools_json, provider_tool_fn fn, void *ud);

/* assistant 轮 tool_calls 遍历：arguments 为 JSON 串原样交付。 */
typedef void (*provider_tool_call_fn)(void *ud, const char *id, const char *name,
                                      const char *arguments);
void prov_tool_each(const char *tool_calls_json, provider_tool_call_fn fn, void *ud);

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

/* usage 段嵌套明细取值：details 对象缺席或键非数字时返回 -1（不改写目标）。 */
static inline long provider_usage_detail(const cJSON *usage, const char *section, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(cJSON_GetObjectItem(usage, section), key);
    return cJSON_IsNumber(v) ? (long)v->valuedouble : -1L;
}

/* usage 段装载（流式/非流式同源）：三值 + reasoning_tokens 双位置回退
 * （顶层或 completion_tokens_details 嵌套）+ prompt_tokens_details.cached_tokens。
 * 存在才覆写、缺失保留现值，与流式多 chunk last-parse-wins 语义同源；
 * 非 object 整段跳过。 */
static inline void provider_usage_load(const cJSON *usage, uint32_t *prompt,
                                       uint32_t *completion, uint32_t *total,
                                       uint32_t *reasoning, uint32_t *cached)
{
    const cJSON *v;
    long n;
    if (!cJSON_IsObject(usage))
        return;
    v = cJSON_GetObjectItem(usage, "prompt_tokens");
    if (cJSON_IsNumber(v))
        *prompt = (uint32_t)v->valuedouble;
    v = cJSON_GetObjectItem(usage, "completion_tokens");
    if (cJSON_IsNumber(v))
        *completion = (uint32_t)v->valuedouble;
    v = cJSON_GetObjectItem(usage, "total_tokens");
    if (cJSON_IsNumber(v))
        *total = (uint32_t)v->valuedouble;
    v = cJSON_GetObjectItem(usage, "reasoning_tokens");
    if (!cJSON_IsNumber(v))
        v = cJSON_GetObjectItem(cJSON_GetObjectItem(usage, "completion_tokens_details"),
                                "reasoning_tokens");
    if (cJSON_IsNumber(v))
        *reasoning = (uint32_t)v->valuedouble;
    n = provider_usage_detail(usage, "prompt_tokens_details", "cached_tokens");
    if (n >= 0)
        *cached = (uint32_t)n;
}

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_ENVELOPE_H */
