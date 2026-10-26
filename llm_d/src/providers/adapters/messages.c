// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file messages.c
 * @brief Messages 线格式适配器：只实现线格式本身——content 块三态消息映射、
 * 具名事件映射、usage 三段求和、x-api-key 鉴权。
 *
 * 端点、默认模型与协议版本头等厂商/协议约定数据一律由 model.yaml 注入
 * （default_* 槽为 NULL，版本头走 extra_headers），本件不持有任何厂商名。
 * 参数段/身份段/tools 遍历归 core/envelope.c，文本推送与工具槽位归
 * core/toolstream.c，编排归 core/adapter.c。
 */

#include "airy_memory.h"
#include "providers/providers_internal.h"
#include "core/toolstream.h"
#include "error.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <cjson_helpers.h>
#include <string.h>

#define MSG_PATH "/messages"
#define MSG_MAX_TOKENS 4096 /* 该线格式必填 max_tokens，调用方未给时兜底 */

/* 请求头声明：x-api-key 鉴权；协议版本头属端点数据，由 extra_headers 注入。 */
static const provider_header_spec_t MSG_HEADERS = {.auth = PROVIDER_AUTH_X_API_KEY};

typedef struct {
    provider_base_ctx_t base;
} messages_ctx_t;

extern const provider_adapter_t messages_ops; /* shim 自引用 ops */

/* prompt 侧总量 = 三段之和：cache 读写不计入 input_tokens，漏加则计费失真。 */
static uint32_t msg_usage(const cJSON *usage)
{
    return provider_json_u32_get(usage, "input_tokens") +
           provider_json_u32_get(usage, "cache_read_input_tokens") +
           provider_json_u32_get(usage, "cache_creation_input_tokens");
}

/* 工具声明改写：Chat {type,function{...}} → {name,description,input_schema}。 */
static void msg_add_tool(void *ud, const char *name, const char *description, const cJSON *params)
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

static void msg_map_tools(cJSON *root, const char *tools_json)
{
    if (!tools_json || !tools_json[0])
        return;
    cJSON *out = cJSON_CreateArray();
    prov_tools_each(tools_json, msg_add_tool, out);
    if (cJSON_GetArraySize(out) > 0)
        cJSON_AddItemToObject(root, "tools", out);
    else
        cJSON_Delete(out);
}

/* 助手轮 tool_calls → tool_use 块：arguments 是 JSON 字符串，须展开为 input 对象。 */
static void msg_add_tool_call(void *ud, const char *id, const char *name, const char *arguments)
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
static void msg_map_msg(cJSON *messages, const llm_message_t *m)
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
        prov_tool_each(m->tool_calls_json, msg_add_tool_call, blocks);
        cJSON_AddStringToObject(msg, "role", "assistant");
    } else {
        cJSON_AddStringToObject(msg, "role", m->role ? m->role : "user");
    }
    cJSON_AddItemToObject(msg, "content", blocks ? blocks : cJSON_CreateString(content));
    cJSON_AddItemToArray(messages, msg);
}

static char *msg_build_request(const llm_request_config_t *manager)
{
    if (!manager)
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    cJSON *root = cJSON_CreateObject();
    if (!root)
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    provider_request_params_fill(root, manager, NULL, MSG_MAX_TOKENS, "stop_sequences");
    msg_map_tools(root, manager->tools_json);
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
        msg_map_msg(messages, m);
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

/* 响应 content 块数组 → 单条 choice：text 块拼接，tool_use 块经 core 槽位
 * 原语装配 tool_calls_json。 */
static void msg_map_content(cJSON *content, llm_message_t *out)
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

static int msg_parse_response(const char *body, llm_response_t **out)
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
            llm_response_free(resp);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        resp->choice_count = 1;
        msg_map_content(content, &resp->choices[0]);
    }
    const cJSON *usage = cJSON_GetObjectItem(root, "usage");
    resp->prompt_tokens = msg_usage(usage);
    resp->completion_tokens = provider_json_u32_get(usage, "output_tokens");
    resp->total_tokens = resp->prompt_tokens + resp->completion_tokens;
    /* cache 命中段已含在三段之和内，另记明细供命中率观测。 */
    resp->cached_tokens = provider_json_u32_get(usage, "cache_read_input_tokens");
    /* norm(NULL) = stop：stop_reason 缺席与 "stop_sequence" 等一并归一。 */
    resp->finish_reason =
        AIRY_STRDUP(llm_finish_reason_norm(provider_json_str_val(root, "stop_reason")));
    *out = resp;
    return AIRY_OK;
}

/* 具名事件映射（feed_event）：事件语义在 data.type，事件行仅作缺席回落；
 * 文本推送与工具槽位均落 core 原语，本件不持累积状态。 */
static int msg_map_event(const char *event, const char *data, size_t data_len, void *user_data)
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
        const cJSON *msg_usage_j = cJSON_GetObjectItem(msg, "usage");
        acc->prompt_tokens = msg_usage(msg_usage_j);
        acc->cached_tokens = provider_json_u32_get(msg_usage_j, "cache_read_input_tokens");
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
        /* stream_take 直取 total_tokens 不求和，总量只能在此得出。 */
        acc->total_tokens = acc->prompt_tokens + acc->completion_tokens;
    }
    return 0;
}

static provider_ctx_t *messages_init(const char *name, const char *api_key, const char *api_base,
                                     const char *organization, double timeout_sec, int max_retries)
{
    messages_ctx_t *ctx = (messages_ctx_t *)AIRY_CALLOC(1, sizeof(messages_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: MESSAGES: INIT-FAIL reason=oom STACK: messages_init");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    provider_base_init(&ctx->base, name, api_key, api_base, organization, timeout_sec, max_retries,
                       NULL);

    if (!ctx->base.api_base[0]) {
        SVC_LOG_ERROR("C-L02: MESSAGES: INIT-FAIL reason=no_base_url provider=%s "
                      "STACK: messages_init",
                      name ? name : "");
        AIRY_FREE(ctx);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "missing api_base");
    }

    SVC_LOG_INFO("C-L02: MESSAGES: INIT api_base=%s timeout=%.1fs retries=%d has_api_key=%d",
                 ctx->base.api_base, ctx->base.timeout_sec, ctx->base.max_retries,
                 ctx->base.api_key[0] ? 1 : 0);
    return (provider_ctx_t *)ctx;
}

static void messages_destroy(provider_ctx_t *ctx_ptr)
{
    if (!ctx_ptr)
        return;
    SVC_LOG_DEBUG("C-L02: MESSAGES: DESTROY ctx=%p", (void *)ctx_ptr);
    AIRY_FREE(ctx_ptr);
}

static int messages_complete(provider_ctx_t *ctx, const llm_request_config_t *manager,
                             llm_response_t **out_response)
{
    return provider_driver_complete(ctx, &messages_ops, manager, out_response);
}

static int msg_complete_stream(provider_ctx_t *ctx, const llm_request_config_t *manager,
                                    llm_stream_callback_t callback, void *callback_data,
                                    llm_response_t **out_response)
{
    return provider_driver_complete_stream(ctx, &messages_ops, manager, callback, callback_data,
                                           out_response);
}

/* 五槽装配：feed_event 非 NULL 唯一决定流式为具名事件模式（core/adapter.c）。 */
const provider_adapter_t messages_ops = {
    .name = "messages", .tag = "MESSAGES",
    .default_model = NULL, .default_base_url = NULL,
    .chat_path = MSG_PATH, .headers = &MSG_HEADERS,
    .init = messages_init, .destroy = messages_destroy,
    .complete = messages_complete, .complete_stream = msg_complete_stream,
    .build_request = msg_build_request, .parse_response = msg_parse_response,
    .feed_event = msg_map_event,
};
