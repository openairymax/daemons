// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file toolstream.c
 * @brief 流式响应装配唯一实现（B16-S3 c5/c6：收敛 openai_stream/
 * deepseek/local 三份逐字同构副本至此）。
 */

#include "toolstream.h"
#include "transport.h"

#include "error.h"

#include <cjson_helpers.h>

#include <string.h>

static provider_tool_slot_t *tool_slot(provider_tool_acc_t *acc, int index)
{
    for (size_t k = 0; k < acc->count; k++) {
        if (acc->slot[k].index == index)
            return &acc->slot[k];
    }
    if (acc->count >= PROVIDER_TOOL_MAX)
        return NULL;
    provider_tool_slot_t *slot = &acc->slot[acc->count++];
    __builtin_memset(slot, 0, sizeof(*slot));
    slot->index = index;
    return slot;
}

static void tool_args_append(provider_tool_slot_t *slot, const char *frag)
{
    size_t flen = strlen(frag);
    if (flen == 0)
        return;
    size_t need = slot->args_len + flen + 1;
    if (need > slot->args_cap) {
        size_t cap = slot->args_cap ? slot->args_cap : 256;
        while (cap < need)
            cap *= 2;
        char *grown = (char *)AIRY_REALLOC(slot->args, cap);
        if (!grown)
            return;
        slot->args = grown;
        slot->args_cap = cap;
    }
    __builtin_memcpy(slot->args + slot->args_len, frag, flen);
    slot->args_len += flen;
    slot->args[slot->args_len] = '\0';
}

void provider_tool_delta(provider_tool_acc_t *acc, cJSON *delta)
{
    cJSON *tcs = cJSON_GetObjectItem(delta, "tool_calls");
    if (!cJSON_IsArray(tcs))
        return;
    int tn = cJSON_GetArraySize(tcs);
    for (int ti = 0; ti < tn; ti++) {
        cJSON *tc = cJSON_GetArrayItem(tcs, ti);
        cJSON *idxj = cJSON_GetObjectItem(tc, "index");
        int idx = cJSON_IsNumber(idxj) ? (int)idxj->valuedouble : (int)acc->count;
        provider_tool_slot_t *slot = tool_slot(acc, idx);
        if (!slot)
            continue;
        cJSON *idj = cJSON_GetObjectItem(tc, "id");
        if (cJSON_IsString(idj) && idj->valuestring && !slot->id[0]) {
            size_t idlen = strlen(idj->valuestring);
            if (idlen >= sizeof(slot->id))
                idlen = sizeof(slot->id) - 1;
            __builtin_memcpy(slot->id, idj->valuestring, idlen);
            slot->id[idlen] = '\0';
        }
        cJSON *fn = cJSON_GetObjectItem(tc, "function");
        if (!cJSON_IsObject(fn))
            continue;
        cJSON *namej = cJSON_GetObjectItem(fn, "name");
        if (cJSON_IsString(namej) && namej->valuestring && !slot->name[0]) {
            size_t nlen = strlen(namej->valuestring);
            if (nlen >= sizeof(slot->name))
                nlen = sizeof(slot->name) - 1;
            __builtin_memcpy(slot->name, namej->valuestring, nlen);
            slot->name[nlen] = '\0';
        }
        cJSON *argj = cJSON_GetObjectItem(fn, "arguments");
        if (cJSON_IsString(argj) && argj->valuestring)
            tool_args_append(slot, argj->valuestring);
    }
}

char *provider_tool_json(const provider_tool_acc_t *acc)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return NULL;
    for (size_t i = 0; i < acc->count; i++) {
        const provider_tool_slot_t *slot = &acc->slot[i];
        cJSON *tc = cJSON_CreateObject();
        cJSON *fn = cJSON_CreateObject();
        if (!tc || !fn) {
            if (tc)
                cJSON_Delete(tc);
            if (fn)
                cJSON_Delete(fn);
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddStringToObject(tc, "id", slot->id[0] ? slot->id : "call_unknown");
        /* OpenAI 续轮必需：tool_calls 元素必须携带 "type":"function"，
         * 缺失时 DeepSeek 等上游对 assistant tool_calls 严格校验并 400
         * （2026-08-16 探针确认：no-type 0B / with-type 200）。 */
        cJSON_AddStringToObject(tc, "type", "function");
        if (slot->name[0])
            cJSON_AddStringToObject(fn, "name", slot->name);
        cJSON_AddStringToObject(fn, "arguments", slot->args ? slot->args : "");
        cJSON_AddItemToObject(tc, "function", fn);
        cJSON_AddItemToArray(arr, tc);
    }
    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return js;
}

void provider_tool_flush(provider_tool_acc_t *acc, llm_response_t *resp,
                         llm_stream_callback_t cb, void *ud)
{
    if (acc->count == 0)
        return;
    char *tc_json = provider_tool_json(acc);
    if (!tc_json)
        return;
    provider_emit_tool_frame(cb, ud, tc_json);
    if (resp && resp->choices && resp->choice_count > 0 && !resp->choices[0].tool_calls_json)
        resp->choices[0].tool_calls_json = tc_json;
    else
        AIRY_FREE(tc_json);
}

void provider_tool_free(provider_tool_acc_t *acc)
{
    for (size_t i = 0; i < acc->count; i++)
        AIRY_FREE(acc->slot[i].args);
    acc->count = 0;
}

void provider_stream_acc_init(provider_stream_acc_t *acc, llm_stream_callback_t cb, void *ud)
{
    __builtin_memset(acc, 0, sizeof(*acc));
    acc->user_cb = cb;
    acc->user_data = ud;
    acc->acc_cap = 4096;
    acc->acc_content = (char *)AIRY_MALLOC(acc->acc_cap);
}

int provider_openai_on_chunk(const char *json_line, void *userdata)
{
    provider_stream_acc_t *acc = (provider_stream_acc_t *)userdata;

    CJSON_PARSE_GUARD(root, json_line, { return 0; });

    if (!acc->resp_id) {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsString(id) && id->valuestring)
            acc->resp_id = AIRY_STRDUP(id->valuestring);
    }

    if (!acc->resp_model) {
        cJSON *model = cJSON_GetObjectItem(root, "model");
        if (cJSON_IsString(model) && model->valuestring)
            acc->resp_model = AIRY_STRDUP(model->valuestring);
    }

    cJSON *created = cJSON_GetObjectItem(root, "created");
    if (cJSON_IsNumber(created) && acc->resp_created == 0)
        acc->resp_created = (uint64_t)created->valuedouble;

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *delta = cJSON_GetObjectItem(choice, "delta");
        if (delta) {
            cJSON *content = cJSON_GetObjectItem(delta, "content");
            if (cJSON_IsString(content) && content->valuestring) {
                if (acc->user_cb)
                    acc->user_cb(content->valuestring, acc->user_data);
                char *grown = provider_buf_append(acc->acc_content, &acc->acc_cap, &acc->acc_len,
                                                  content->valuestring);
                if (grown)
                    acc->acc_content = grown;
            }

            /* Reasoning trace arrives in the same delta stream (DeepSeek
             * reasoner/Kimi). Forward each delta immediately as an RS 'R'
             * control frame so IPC clients can show the thinking chain
             * live, while still accumulating it for the assembled response
             * (tool-loop echo). Endpoints without reasoning_content bypass
             * this branch naturally. */
            cJSON *reasoning = cJSON_GetObjectItem(delta, "reasoning_content");
            if (cJSON_IsString(reasoning) && reasoning->valuestring) {
                if (acc->user_cb)
                    provider_emit_reasoning_frame(acc->user_cb, acc->user_data,
                                                  reasoning->valuestring);
                char *grown = provider_buf_append(acc->acc_reasoning, &acc->acc_reasoning_cap,
                                                  &acc->acc_reasoning_len,
                                                  reasoning->valuestring);
                if (grown)
                    acc->acc_reasoning = grown;
            }

            provider_tool_delta(&acc->tools, delta);
        }

        cJSON *fr = cJSON_GetObjectItem(choice, "finish_reason");
        if (cJSON_IsString(fr) && fr->valuestring && strcmp(fr->valuestring, "null") != 0) {
            AIRY_FREE(acc->finish_reason);
            acc->finish_reason = AIRY_STRDUP(llm_finish_reason_norm(fr->valuestring));
        }
    }

    /* Streaming usage: OpenAI (stream_options.include_usage) / DeepSeek /
     * local OpenAI-compatible endpoints attach the usage block in the final
     * chunk, which has no choices. Last parse wins (usage is most complete
     * at stream end). */
    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
        cJSON *tt = cJSON_GetObjectItem(usage, "total_tokens");
        if (cJSON_IsNumber(pt))
            acc->prompt_tokens = (uint32_t)pt->valuedouble;
        if (cJSON_IsNumber(ct))
            acc->completion_tokens = (uint32_t)ct->valuedouble;
        if (cJSON_IsNumber(tt))
            acc->total_tokens = (uint32_t)tt->valuedouble;
        cJSON *rt = cJSON_GetObjectItem(usage, "reasoning_tokens");
        if (!cJSON_IsNumber(rt)) {
            cJSON *details = cJSON_GetObjectItem(usage, "completion_tokens_details");
            if (cJSON_IsObject(details))
                rt = cJSON_GetObjectItem(details, "reasoning_tokens");
        }
        if (cJSON_IsNumber(rt))
            acc->reasoning_tokens = (uint32_t)rt->valuedouble;
    }

    return 0;
}

llm_response_t *provider_openai_stream_take(provider_stream_acc_t *acc)
{
    llm_response_t *resp = (llm_response_t *)AIRY_CALLOC(1, sizeof(llm_response_t));
    if (!resp) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }

    resp->id = acc->resp_id ? acc->resp_id : AIRY_STRDUP("");
    acc->resp_id = NULL;
    resp->model = acc->resp_model ? acc->resp_model : AIRY_STRDUP("unknown");
    acc->resp_model = NULL;
    resp->created = acc->resp_created;
    resp->choices = (llm_message_t *)AIRY_CALLOC(1, sizeof(llm_message_t));
    if (resp->choices) {
        resp->choice_count = 1;
        resp->choices[0].role = AIRY_STRDUP("assistant");
        resp->choices[0].content = acc->acc_content;
        acc->acc_content = NULL;
        resp->choices[0].reasoning_content = acc->acc_reasoning;
        acc->acc_reasoning = NULL;
    } else {
        resp->choice_count = 0;
    }
    resp->finish_reason = acc->finish_reason ? acc->finish_reason : AIRY_STRDUP(LLM_FINISH_STOP);
    acc->finish_reason = NULL;
    resp->prompt_tokens = acc->prompt_tokens;
    resp->completion_tokens = acc->completion_tokens;
    resp->total_tokens = acc->total_tokens;
    resp->reasoning_tokens = acc->reasoning_tokens;
    return resp;
}

void provider_stream_acc_free(provider_stream_acc_t *acc)
{
    AIRY_FREE(acc->acc_content);
    AIRY_FREE(acc->acc_reasoning);
    AIRY_FREE(acc->resp_id);
    AIRY_FREE(acc->resp_model);
    AIRY_FREE(acc->finish_reason);
    provider_tool_free(&acc->tools);
}
