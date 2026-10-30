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

/* 可选采样/扩展段：流式 usage 声明与两种惩罚项。缺省值不写键，
 * 保持旧请求体字节级等价。 */
static void opt_attach(cJSON *root, const llm_request_config_t *m)
{
    if (m->stream) {
        /* 流式请求必须显式声明 include_usage：chat 兼容端点
         * （vLLM/llama.cpp 等）默认不回传 usage，不声明则 token 计数与
         * 计费恒为 0。已回传的端点显式声明后幂等。 */
        cJSON *so = cJSON_CreateObject();

        if (so) {
            cJSON_AddBoolToObject(so, "include_usage", 1);
            cJSON_AddItemToObject(root, "stream_options", so);
        }
    }
    if (m->presence_penalty != 0)
        cJSON_AddNumberToObject(root, "presence_penalty", m->presence_penalty);
    if (m->frequency_penalty != 0)
        cJSON_AddNumberToObject(root, "frequency_penalty", m->frequency_penalty);
}

/* 工具声明段：仅当上游给出非空数组时整段透传。 */
static void tools_attach(cJSON *root, const char *tools_json)
{
    if (!tools_json || !tools_json[0])
        return;

    CJSON_PARSE_GUARD(tools, tools_json, { return; });
    if (cJSON_IsArray(tools) && cJSON_GetArraySize(tools) > 0)
        cJSON_AddItemToObject(root, "tools", cJSON_Duplicate(tools, 1));
}

/* 单条消息：role/content 恒发；reasoning_content、tool_call_id 与
 * tool_calls 仅在给出时回传。 */
static cJSON *msg_build(const llm_message_t *m)
{
    cJSON *msg = cJSON_CreateObject();

    if (!msg)
        return NULL;

    cJSON_AddStringToObject(msg, "role", m->role ? m->role : "user");
    cJSON_AddStringToObject(msg, "content", m->content ? m->content : "");

    /* 推理型端点要求把 assistant 轮的 reasoning_content 在重发时
     * 原样回传；工具多轮之间丢弃该字段会招致上游 HTTP 400。 */
    if (m->reasoning_content && m->reasoning_content[0])
        cJSON_AddStringToObject(msg, "reasoning_content", m->reasoning_content);

    if (m->tool_call_id && m->tool_call_id[0])
        cJSON_AddStringToObject(msg, "tool_call_id", m->tool_call_id);

    if (m->tool_calls_json && m->tool_calls_json[0]) {
        CJSON_PARSE_GUARD(tc, m->tool_calls_json, {});
        if (cJSON_IsArray(tc))
            cJSON_AddItemToObject(msg, "tool_calls", cJSON_Duplicate(tc, 1));
    }
    return msg;
}

static cJSON *msgs_build(const llm_request_config_t *m)
{
    cJSON *msgs = cJSON_CreateArray();

    if (!msgs)
        return NULL;

    for (size_t i = 0; i < m->message_count; ++i) {
        cJSON *msg = msg_build(&m->messages[i]);

        if (msg)
            cJSON_AddItemToArray(msgs, msg);
    }
    return msgs;
}

/* 通用供应商扩展段：逐键并入顶层，键冲突时先删旧键再挂新键
 * （扩展段为准）。空串 / 非法 JSON 静默跳过，不阻断请求装配。 */
static void extra_merge(cJSON *root, const char *extra_json)
{
    if (!extra_json || !extra_json[0])
        return;

    CJSON_PARSE_GUARD(extra, extra_json, { return; });
    if (!cJSON_IsObject(extra))
        return;

    const cJSON *field = NULL;

    cJSON_ArrayForEach(field, extra)
    {
        if (field->string) {
            cJSON_DeleteItemFromObject(root, field->string);
            cJSON_AddItemToObject(root, field->string, cJSON_Duplicate(field, 1));
        }
    }
}

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

    opt_attach(root, manager);
    tools_attach(root, manager->tools_json);
    cJSON_AddItemToObject(root, "messages", msgs_build(manager));
    extra_merge(root, extra_json);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* 单条 choice 的 message 段：四个字段各自独立可选，缺省留 NULL。 */
static void msg_load(llm_message_t *dst, const cJSON *message)
{
    if (!message)
        return;

    const cJSON *role = cJSON_GetObjectItem(message, "role");
    const cJSON *content = cJSON_GetObjectItem(message, "content");
    const cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning_content");
    const cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");

    if (cJSON_IsString(role) && role->valuestring)
        dst->role = AIRY_STRDUP(role->valuestring);
    if (cJSON_IsString(content) && content->valuestring)
        dst->content = AIRY_STRDUP(content->valuestring);
    if (cJSON_IsString(reasoning) && reasoning->valuestring)
        dst->reasoning_content = AIRY_STRDUP(reasoning->valuestring);
    if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0)
        dst->tool_calls_json = cJSON_PrintUnformatted(tool_calls);
}

/* choices 数组整体装载：finish_reason 取首个非空值，与旧行为一致。
 * 返回 0 成功，-1 表示 choice 数组分配失败（调用方负责释放 resp）。 */
static int choices_load(llm_response_t *resp, const cJSON *choices)
{
    int size = cJSON_GetArraySize(choices);

    resp->choice_count = (size_t)size;
    resp->choices = (llm_message_t *)AIRY_CALLOC((size_t)size, sizeof(llm_message_t));
    if (!resp->choices) {
        SVC_LOG_ERROR("C-L02: PROVIDER: PARSE-FAIL reason=oom_choices "
                      "STACK: prov_parse_chat");
        return -1;
    }

    for (int i = 0; i < size; ++i) {
        cJSON *choice = cJSON_GetArrayItem(choices, i);
        cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");

        msg_load(&resp->choices[i], cJSON_GetObjectItem(choice, "message"));
        if (!resp->finish_reason && cJSON_IsString(finish) && finish->valuestring)
            resp->finish_reason = AIRY_STRDUP(llm_finish_reason_norm(finish->valuestring));
    }
    return 0;
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
    if (cJSON_IsArray(choices) && choices_load(resp, choices) != 0) {
        llm_response_free(resp);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    provider_usage_load(cJSON_GetObjectItem(root, "usage"), &resp->prompt_tokens,
                        &resp->completion_tokens, &resp->total_tokens,
                        &resp->reasoning_tokens, &resp->cached_tokens);
    provider_timings_load(cJSON_GetObjectItem(root, "timings"), &resp->timings);

    *out = resp;
    return AIRY_OK;
}

/* C3 thinking 控制：chat_template_kwargs 容器惰性创建（vLLM/llama.cpp
 * 官方线格式——模板变量透传，Qwen3/GLM 等推理模型的思考开关经此承载）。
 * 返回 NULL 时调用方静默跳过（宁可不发不乱发）。 */
static cJSON *ctk_ensure(cJSON *root)
{
    cJSON *ctk = cJSON_GetObjectItem(root, "chat_template_kwargs");
    if (!ctk) {
        ctk = cJSON_CreateObject();
        if (ctk)
            cJSON_AddItemToObject(root, "chat_template_kwargs", ctk);
    }
    return ctk;
}

/* 基础采样面：模型名、温度、最大 token、top_p。C11：temperature_set
 * 显式哨兵允许下发 0.0；未显式设置时维持旧请求体等价（>0 用之，否则
 * 0.7 兜底），旧调用方零感知。 */
static void fill_core(cJSON *root, const llm_request_config_t *m, const char *default_model,
                      int default_max_tokens)
{
    const char *model = (m->model && m->model[0]) ? m->model : default_model;
    double temp = (m->temperature_set || m->temperature > 0.0f) ? (double)m->temperature : 0.7;
    int max_tokens = m->max_tokens > 0 ? m->max_tokens : default_max_tokens;

    cJSON_AddStringToObject(root, "model", model ? model : "");
    cJSON_AddNumberToObject(root, "temperature", temp);
    if (max_tokens > 0)
        cJSON_AddNumberToObject(root, "max_tokens", max_tokens);
    if (m->top_p > 0)
        cJSON_AddNumberToObject(root, "top_p", m->top_p);
}

/* C11 能力位门控：caps 由 driver 盖印，位为假即该厂商族不支持，显式
 * 请求也被丢弃（宁可不发不乱发）；位为真且值非零值才下发。T2 KV 复用
 * 透传：缓存声明随能力位恒发，槽位亲和仅显式设置才下发。 */
static void fill_caps(cJSON *root, const llm_request_config_t *m)
{
    if (m->caps.accepts_seed && m->seed != 0u)
        cJSON_AddNumberToObject(root, "seed", (double)m->seed);
    if (m->caps.accepts_grammar && m->grammar && m->grammar[0])
        cJSON_AddStringToObject(root, "grammar", m->grammar);
    if (m->caps.accepts_response_format && m->response_format == LLM_RF_JSON_OBJECT) {
        cJSON *rf = cJSON_CreateObject();

        if (rf) {
            cJSON_AddStringToObject(rf, "type", "json_object");
            cJSON_AddItemToObject(root, "response_format", rf);
        }
    }
    if (m->caps.accepts_num_ctx && m->num_ctx != 0u)
        cJSON_AddNumberToObject(root, "num_ctx", (double)m->num_ctx);
    if (m->caps.accepts_cache_prompt)
        cJSON_AddBoolToObject(root, "cache_prompt", 1);
    if (m->caps.accepts_slot_id && m->slot_set)
        cJSON_AddNumberToObject(root, "slot_id", (double)m->slot_id);
}

/* C3 thinking 控制：显式设置且能力位置位才下发；缺省旧请求体字节级
 * 不变。enable_thinking 为布尔语义（false 也是有效值，哨兵位使其可
 * 表达），budget 零值 = 未设置。 */
static void fill_thinking(cJSON *root, const llm_request_config_t *m)
{
    if (m->caps.accepts_enable_thinking && m->thinking_set) {
        cJSON *ctk = ctk_ensure(root);

        if (ctk)
            cJSON_AddBoolToObject(ctk, "enable_thinking", m->enable_thinking ? 1 : 0);
    }
    if (m->caps.accepts_thinking_budget && m->thinking_budget != 0u) {
        cJSON *ctk = ctk_ensure(root);

        if (ctk)
            cJSON_AddNumberToObject(ctk, "thinking_budget", (double)m->thinking_budget);
    }
}

/* 流式与停止序列：stop_key 按厂商线格式择键（stop / stop_sequence）。 */
static void fill_stop(cJSON *root, const llm_request_config_t *m, const char *stop_key)
{
    if (m->stream)
        cJSON_AddBoolToObject(root, "stream", 1);
    if (m->stop_count > 0 && m->stop) {
        cJSON *stop = cJSON_CreateArray();

        for (size_t i = 0; i < m->stop_count; ++i)
            cJSON_AddItemToArray(stop, cJSON_CreateString(m->stop[i]));
        cJSON_AddItemToObject(root, stop_key, stop);
    }
}

void provider_request_params_fill(cJSON *root, const llm_request_config_t *manager,
                                  const char *default_model, int default_max_tokens,
                                  const char *stop_key)
{
    if (!root || !manager || !stop_key)
        return;
    fill_core(root, manager, default_model, default_max_tokens);
    fill_caps(root, manager);
    fill_thinking(root, manager);
    fill_stop(root, manager, stop_key);
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
