// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file anthropic.c
 * @brief Anthropic Messages API 适配（B16-S3.3）：仅剩厂商差异面——content 块三态
 * 消息映射、具名事件映射、usage 三段求和。编排归 core/adapter.c，参数段/身份段/
 * tools 遍历归 core/envelope.c，文本推送与工具槽位归 core/toolstream.c。
 */

#include "airy_memory.h"
#include "core/adapter.h"
#include "core/toolstream.h"
#include "error.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <cjson_helpers.h>
#include <string.h>

#define ANTHROPIC_DEFAULT_BASE "https://api.anthropic.com/v1"
#define ANTHROPIC_DEFAULT_MODEL "claude-3-sonnet-20240229"
#define ANTHROPIC_CHAT_PATH "/messages"
#define ANTHROPIC_MAX_TOKENS 4096 /* Messages API 必填 max_tokens，调用方未给时兜底 */

static const char *const ANTHROPIC_EXTRA[] = {"anthropic-version: 2023-06-01"};
static const provider_header_spec_t ANTHROPIC_HEADERS = {.auth = PROVIDER_AUTH_X_API_KEY,
                                                         .extra = ANTHROPIC_EXTRA,
                                                         .extra_count = 1};

typedef struct {
    provider_base_ctx_t base;
} anthropic_ctx_t;

extern const provider_adapter_t anthropic_ops; /* shim 自引用 ops */

/* prompt 侧总量 = 三段之和：cache 读写不计入 input_tokens，漏加则计费失真。 */
static uint32_t ant_map_usage(const cJSON *usage)
{
    return provider_json_u32_get(usage, "input_tokens") +
           provider_json_u32_get(usage, "cache_read_input_tokens") +
           provider_json_u32_get(usage, "cache_creation_input_tokens");
}

/* 工具声明改写：OpenAI {type,function{...}} → {name,description,input_schema}。 */
static void ant_add_tool(void *ud, const char *name, const char *description, const cJSON *params)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", name);
    if (description)
        cJSON_AddStringToObject(tool, "description", description);
    cJSON_AddItemToObject(tool, "input_schema",
                          cJSON_IsObject(params) ? cJSON_Duplicate(params, 1)
                                                 : cJSON_CreateObject());
    cJSON_AddItemToArray((cJSON *)ud, tool);
}

static void ant_map_tools(cJSON *root, const char *tools_json)
{
    if (!tools_json || !tools_json[0])
        return;
    cJSON *out = cJSON_CreateArray();
    provider_openai_tools_foreach(tools_json, ant_add_tool, out);
    if (cJSON_GetArraySize(out) > 0)
        cJSON_AddItemToObject(root, "tools", out);
    else
        cJSON_Delete(out);
}

/* 助手轮 tool_calls → tool_use 块：arguments 是 JSON 字符串，须展开为 input 对象。 */
static void ant_add_tool_call(void *ud, const char *id, const char *name, const char *arguments)
{
    cJSON *blk = cJSON_CreateObject();
    cJSON_AddStringToObject(blk, "type", "tool_use");
    cJSON_AddStringToObject(blk, "id", id);
    cJSON_AddStringToObject(blk, "name", name);
    cJSON *input = arguments ? cJSON_Parse(arguments) : NULL;
    cJSON_AddItemToObject(blk, "input", input ? input : cJSON_CreateObject());
    cJSON_AddItemToArray((cJSON *)ud, blk);
}

/* 单条消息三态：工具结果回注走 user 角色 tool_result 块、助手轮走 text+tool_use
 * 块数组，其余走 role + 字符串 content。 */
static void ant_map_msg(cJSON *messages, const llm_message_t *m)
{
    const char *content = m->content ? m->content : "";
    cJSON *msg = cJSON_CreateObject();
    cJSON *blocks = NULL;
    if (m->tool_call_id && m->tool_call_id[0]) {
        cJSON *blk = cJSON_CreateObject();
        cJSON_AddStringToObject(blk, "type", "tool_result");
        cJSON_AddStringToObject(blk, "tool_use_id", m->tool_call_id);
        cJSON_AddStringToObject(blk, "content", content);
        blocks = cJSON_CreateArray();
        cJSON_AddItemToArray(blocks, blk);
        cJSON_AddStringToObject(msg, "role", "user");
    } else if (m->tool_calls_json && m->tool_calls_json[0]) {
        blocks = cJSON_CreateArray();
        if (content[0]) {
            cJSON *text = cJSON_CreateObject();
            cJSON_AddStringToObject(text, "type", "text");
            cJSON_AddStringToObject(text, "text", content);
            cJSON_AddItemToArray(blocks, text);
        }
        provider_openai_tool_calls_foreach(m->tool_calls_json, ant_add_tool_call, blocks);
        cJSON_AddStringToObject(msg, "role", "assistant");
    } else {
        cJSON_AddStringToObject(msg, "role", m->role ? m->role : "user");
    }
    cJSON_AddItemToObject(msg, "content", blocks ? blocks : cJSON_CreateString(content));
    cJSON_AddItemToArray(messages, msg);
}

static char *anthropic_build_request(const llm_request_config_t *manager)
{
    if (!manager)
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    cJSON *root = cJSON_CreateObject();
    if (!root)
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    provider_request_params_fill(root, manager, ANTHROPIC_DEFAULT_MODEL, ANTHROPIC_MAX_TOKENS,
                                 "stop_sequences");
    ant_map_tools(root, manager->tools_json);
    /* system 是顶层单一字符串而非 messages 成员，故不入数组、只留末条。 */
    char *system = NULL;
    cJSON *messages = cJSON_CreateArray();
    for (size_t i = 0; i < manager->message_count; ++i) {
        const llm_message_t *m = &manager->messages[i];
        if (m->role && strcmp(m->role, "system") == 0) {
            AIRY_FREE(system);
            system = AIRY_STRDUP(m->content ? m->content : "");
            continue;
        }
        ant_map_msg(messages, m);
    }
    if (system) {
        cJSON_AddStringToObject(root, "system", system);
        AIRY_FREE(system);
    }
    cJSON_AddItemToObject(root, "messages", messages);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* 响应 content 块数组 → OpenAI 形状单条 choice：text 块拼接，tool_use 块经
 * core 槽位原语装配 tool_calls_json。 */
static void ant_map_content(cJSON *content, llm_message_t *out)
{
    provider_tool_acc_t tools = {0};
    char *text = NULL;
    size_t cap = 0, len = 0;
    int index = 0, count = cJSON_GetArraySize(content);
    for (int i = 0; i < count; ++i) {
        cJSON *blk = cJSON_GetArrayItem(content, i);
        const char *btype = provider_json_str_val(blk, "type");
        if (!btype)
            continue;
        if (strcmp(btype, "text") == 0) {
            const char *t = provider_json_str_val(blk, "text");
            if (t)
                text = provider_buf_append(text, &cap, &len, t);
        } else if (strcmp(btype, "tool_use") == 0) {
            char *args = cJSON_PrintUnformatted(cJSON_GetObjectItem(blk, "input"));
            provider_tool_begin(&tools, index, provider_json_str_val(blk, "id"),
                                provider_json_str_val(blk, "name"));
            provider_tool_args_add(&tools, index, args ? args : "");
            AIRY_FREE(args);
            ++index;
        }
    }
    out->role = AIRY_STRDUP("assistant");
    out->content = text ? text : AIRY_STRDUP("");
    if (tools.count > 0)
        out->tool_calls_json = provider_tool_json(&tools);
    provider_tool_free(&tools);
}

static int anthropic_parse_response(const char *body, llm_response_t **out)
{
    if (!body || !out)
        return AIRY_ERR_INVALID_PARAM;
    CJSON_PARSE_GUARD(root, body, { return AIRY_ERR_PARSE_ERROR; });
    llm_response_t *resp = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp)
        return AIRY_ERR_OUT_OF_MEMORY;
    provider_response_identity_load(resp, root);
    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (cJSON_IsArray(content) && cJSON_GetArraySize(content) > 0) {
        resp->choices = (llm_message_t *)AIRY_CALLOC(1, sizeof(llm_message_t));
        if (!resp->choices) {
            provider_response_free(resp);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        resp->choice_count = 1;
        ant_map_content(content, &resp->choices[0]);
    }
    const cJSON *usage = cJSON_GetObjectItem(root, "usage");
    resp->prompt_tokens = ant_map_usage(usage);
    resp->completion_tokens = provider_json_u32_get(usage, "output_tokens");
    resp->total_tokens = resp->prompt_tokens + resp->completion_tokens;
    /* norm(NULL) = stop：stop_reason 缺席与 "stop_sequence" 等一并归一。 */
    resp->finish_reason =
        AIRY_STRDUP(llm_finish_reason_norm(provider_json_str_val(root, "stop_reason")));
    *out = resp;
    return AIRY_OK;
}

/* 具名事件映射（feed_event）：事件语义在 data.type，事件行仅作缺席回落；
 * 文本推送与工具槽位均落 core 原语，本件不持累积状态。 */
static int ant_map_event(const char *event, const char *data, size_t data_len, void *user_data)
{
    provider_stream_acc_t *acc = (provider_stream_acc_t *)user_data;
    if (!data || data_len == 0)
        return 0;
    CJSON_PARSE_GUARD(root, data, { return 0; });
    const char *name = provider_json_str_val(root, "type");
    if (!name)
        name = event ? event : "";
    const cJSON *delta = cJSON_GetObjectItem(root, "delta");
    int index = (int)provider_json_u32_get(root, "index");
    if (strcmp(name, "message_start") == 0) {
        const cJSON *msg = cJSON_GetObjectItem(root, "message");
        provider_json_str_set(msg, "id", &acc->resp_id);
        provider_json_str_set(msg, "model", &acc->resp_model);
        acc->prompt_tokens = ant_map_usage(cJSON_GetObjectItem(msg, "usage"));
    } else if (strcmp(name, "content_block_start") == 0) {
        const cJSON *blk = cJSON_GetObjectItem(root, "content_block");
        if (provider_json_str_val(blk, "name"))
            provider_tool_begin(&acc->tools, index, provider_json_str_val(blk, "id"),
                                provider_json_str_val(blk, "name"));
    } else if (strcmp(name, "content_block_delta") == 0) {
        const char *dname = provider_json_str_val(delta, "type");
        if (dname && strcmp(dname, "text_delta") == 0)
            provider_stream_text_push(acc, provider_json_str_val(delta, "text"));
        else if (dname && strcmp(dname, "input_json_delta") == 0)
            provider_tool_args_add(&acc->tools, index,
                                   provider_json_str_val(delta, "partial_json"));
    } else if (strcmp(name, "message_delta") == 0) {
        const char *fr = provider_json_str_val(delta, "stop_reason");
        if (fr) {
            AIRY_FREE(acc->finish_reason);
            acc->finish_reason = AIRY_STRDUP(llm_finish_reason_norm(fr));
        }
        const cJSON *usage = cJSON_GetObjectItem(root, "usage");
        acc->completion_tokens = provider_json_u32_get(usage, "output_tokens");
        /* stream_take 直取 total_tokens 不求和，anthropic 总量只能在此得出。 */
        acc->total_tokens = acc->prompt_tokens + acc->completion_tokens;
    }
    return 0;
}

static provider_ctx_t *anthropic_init(const char *name, const char *api_key, const char *api_base,
                                      const char *organization, double timeout_sec, int max_retries)
{
    anthropic_ctx_t *ctx = (anthropic_ctx_t *)AIRY_CALLOC(1, sizeof(anthropic_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: ANTHROPIC: INIT-FAIL reason=oom STACK: anthropic_init");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    provider_base_init(&ctx->base, name, api_key, api_base, organization, timeout_sec, max_retries,
                       ANTHROPIC_DEFAULT_BASE);
    SVC_LOG_INFO("C-L02: ANTHROPIC: INIT api_base=%s timeout=%.1fs retries=%d has_api_key=%d",
                 ctx->base.api_base, ctx->base.timeout_sec, ctx->base.max_retries,
                 ctx->base.api_key[0] ? 1 : 0);
    return (provider_ctx_t *)ctx;
}

static void anthropic_destroy(provider_ctx_t *ctx_ptr)
{
    if (!ctx_ptr)
        return;
    SVC_LOG_DEBUG("C-L02: ANTHROPIC: DESTROY ctx=%p", (void *)ctx_ptr);
    AIRY_FREE(ctx_ptr);
}

static int anthropic_complete(provider_ctx_t *ctx, const llm_request_config_t *manager,
                              llm_response_t **out_response)
{
    return provider_driver_complete(ctx, &anthropic_ops, manager, out_response);
}

static int anthropic_complete_stream(provider_ctx_t *ctx, const llm_request_config_t *manager,
                                     llm_stream_callback_t callback, void *callback_data,
                                     llm_response_t **out_response)
{
    return provider_driver_complete_stream(ctx, &anthropic_ops, manager, callback, callback_data,
                                           out_response);
}

/* 五槽装配：feed_event 非 NULL 唯一决定流式为具名事件模式（core/adapter.c）。 */
const provider_adapter_t anthropic_ops = {
    .name = "anthropic", .tag = "ANTHROPIC",
    .default_model = ANTHROPIC_DEFAULT_MODEL, .default_base_url = ANTHROPIC_DEFAULT_BASE,
    .chat_path = ANTHROPIC_CHAT_PATH, .headers = &ANTHROPIC_HEADERS,
    .init = anthropic_init, .destroy = anthropic_destroy,
    .complete = anthropic_complete, .complete_stream = anthropic_complete_stream,
    .build_request = anthropic_build_request, .parse_response = anthropic_parse_response,
    .feed_event = ant_map_event,
};
