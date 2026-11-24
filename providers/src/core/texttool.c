// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file texttool.c
 * @brief 文本形态工具调用补全（0.1.21 P1-8）。
 *
 * 只处理 <tool_call>...</tool_call> 显式标记：本地模型把调用写进 content 时，
 * 原生 tool_calls 为空，工具回路会误判为「本轮不需要工具」。本件把标记内的
 * JSON 还原成标准 tool_calls 数组，并归一 finish_reason，使回路照常执行。
 *
 * 不做自由文本猜测，也不匹配裸 JSON：那会把示例代码/数据误当成调用。
 */

#include "airy_prov/provider_texttool.h"

#include "airy_memory.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <string.h>

#define TT_OPEN "<tool_call>"
#define TT_CLOSE "</tool_call>"
#define TT_OPEN_LEN 11
#define TT_CLOSE_LEN 12

/* 块内允许 ```json 围栏（部分模板会包一层），去围栏后仍按 JSON 解析。 */
static char *block_dup(const char *p, size_t len)
{
    while (len > 0 && (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')) {
        p++;
        len--;
    }
    if (len >= 3 && p[0] == '`' && p[1] == '`' && p[2] == '`') {
        size_t skip = 3;
        if (len >= 7 && (p[3] == 'j' || p[3] == 'J'))
            skip = 7; /* ```json */
        /* 只在存在闭合围栏时裁剪；否则保留原文，避免把 JSON 整体剥空。 */
        const char *fence = NULL;
        for (size_t i = skip; i + 3 <= len; i++) {
            if (p[i] == '`' && p[i + 1] == '`' && p[i + 2] == '`') {
                fence = p + i;
                break;
            }
        }
        if (fence) {
            p += skip;
            len = (size_t)(fence - p);
        }
    }
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' '))
        len--;
    char *out = (char *)AIRY_MALLOC(len + 1);
    if (!out)
        return NULL;
    if (len)
        __builtin_memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

static const char *pick_name(const cJSON *obj)
{
    static const char *keys[] = {"name", "tool", "tool_name"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *v = cJSON_GetObjectItem(obj, keys[i]);
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0])
            return v->valuestring;
    }
    return NULL;
}

/* arguments 三种形态：对象（直接序列化）、字符串（已是 JSON 串）、缺席（{}）。 */
static char *pick_args(const cJSON *obj)
{
    static const char *keys[] = {"arguments", "parameters", "input", "args"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *v = cJSON_GetObjectItem(obj, keys[i]);
        if (!v)
            continue;
        if (cJSON_IsString(v))
            return AIRY_STRDUP(v->valuestring ? v->valuestring : "{}");
        return cJSON_PrintUnformatted(v);
    }
    return AIRY_STRDUP("{}");
}

/* 解析单个块；成功追加一条标准 function 调用并返回 1。 */
static int parse_block(const char *p, size_t len, cJSON *arr)
{
    char *buf = block_dup(p, len);
    if (!buf)
        return 0;
    cJSON *obj = cJSON_Parse(buf);
    AIRY_FREE(buf);
    if (!cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        return 0;
    }
    /* 两种块形态：扁平 {name,arguments} 与 {function:{name,arguments}}。 */
    const cJSON *fnobj = cJSON_GetObjectItem(obj, "function");
    const cJSON *src = cJSON_IsObject(fnobj) ? fnobj : obj;
    const char *name = pick_name(src);
    if (!name) {
        cJSON_Delete(obj);
        return 0;
    }
    char *args = pick_args(src);
    cJSON *tc = cJSON_CreateObject();
    cJSON *fn = cJSON_CreateObject();
    if (!tc || !fn || !args) {
        cJSON_Delete(tc);
        cJSON_Delete(fn);
        cJSON_Delete(obj);
        AIRY_FREE(args);
        return 0;
    }
    cJSON_AddStringToObject(tc, "type", "function");
    cJSON_AddStringToObject(fn, "name", name);
    cJSON_AddStringToObject(fn, "arguments", args);
    cJSON_AddItemToObject(tc, "function", fn);
    cJSON_AddItemToArray(arr, tc);
    AIRY_FREE(args);
    cJSON_Delete(obj);
    return 1;
}

/* 扫描一段 content 的全部 <tool_call> 块。 */
static int parse_content(const char *content, cJSON *arr)
{
    int added = 0;
    const char *p = content;
    while ((p = strstr(p, TT_OPEN)) != NULL) {
        p += TT_OPEN_LEN;
        const char *end = strstr(p, TT_CLOSE);
        if (!end)
            break;
        added += parse_block(p, (size_t)(end - p), arr);
        p = end + TT_CLOSE_LEN;
    }
    return added;
}

int prov_text_tool(llm_response_t *resp)
{
    if (!resp || !resp->choices)
        return 0;

    int total = 0;
    for (size_t i = 0; i < resp->choice_count; i++) {
        llm_message_t *m = &resp->choices[i];
        if (m->tool_calls_json && m->tool_calls_json[0])
            continue;
        if (!m->content || !strstr(m->content, TT_OPEN))
            continue;

        cJSON *arr = cJSON_CreateArray();
        if (!arr)
            continue;
        int n = parse_content(m->content, arr);
        if (n <= 0) {
            cJSON_Delete(arr);
            continue;
        }
        char *js = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        if (!js)
            continue;
        m->tool_calls_json = js;
        /* 原生 finish_reason 为 stop/空时归一为 tool_calls：上层据此判定
         * 「本轮让位给工具」，而不是把带调用的文本当最终答案直接返回。 */
        if (!resp->finish_reason || strcmp(resp->finish_reason, LLM_FINISH_STOP) == 0) {
            char *fr = AIRY_STRDUP(LLM_FINISH_TOOL_CALLS);
            if (fr) {
                AIRY_FREE(resp->finish_reason);
                resp->finish_reason = fr;
            }
        }
        SVC_LOG_WARN("C-L02: texttool: TEXT-TOOL-FALLBACK model=%s calls=%d "
                     "reason=native_tool_calls_absent",
                     resp->model ? resp->model : "unknown", n);
        total += n;
    }
    return total;
}
