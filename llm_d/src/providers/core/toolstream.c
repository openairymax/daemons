// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file toolstream.c
 * @brief 流式 tool_calls 装配唯一实现（B16-S3 c5：收敛 openai_stream/
 * deepseek/local 三份逐字同构副本至此）。
 */

#include "toolstream.h"
#include "transport.h"

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
