// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_run_loop.c
 * @brief Agent run 工具循环（ReAct，M1-1a 引擎下沉）。
 *
 * 自 gateway_biz_llm.c 迁移：LLM complete -> tool_calls -> tool_d 执行
 * -> 回填上下文 -> 继续。工具 schema 单一权威经 commons 契约层
 * （airy_tool_schema.h）获取；llm_d/tool_d 经 daemon_rpc_client 直连。
 */

#include "agent_run_internal.h"

#include "airy_memory.h"
#include "airy_tool_schema.h"
#include "daemon_rpc_client.h"
#include "platform.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGENT_RUN_SOCK_BUF AIRY_PATH_MAX

/* 构建 llm_d complete 请求（透传 tools 数组与常规生成参数）。 */
static char *run_build_llm_params(const char *model, const cJSON *messages)
{
    cJSON *params = cJSON_CreateObject();
    if (!params)
        return NULL;
    cJSON_AddStringToObject(params, "model", model);
    cJSON_AddItemToObject(params, "messages", cJSON_Duplicate(messages, 1));
    cJSON *tools = cJSON_Parse(AIRY_TOOLS_JSON_SOURCE);
    if (tools)
        cJSON_AddItemToObject(params, "tools", tools);
    cJSON_AddNumberToObject(params, "max_tokens", 2048);
    cJSON_AddNumberToObject(params, "temperature", 0.7);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    return params_str;
}

/* 解析 llm_d 响应的文本/推理/用量。 */
static int run_parse_result(const char *llm_resp, char **out_text, uint64_t *out_tokens,
                            double *out_cost, char **out_reasoning)
{
    *out_text = NULL;
    *out_tokens = 0;
    if (out_cost)
        *out_cost = 0.0;
    if (out_reasoning)
        *out_reasoning = NULL;

    cJSON *root = cJSON_Parse(llm_resp);
    if (!root)
        return -1;
    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 =
        (choices && cJSON_GetArraySize(choices) > 0) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *content = choice0 ? cJSON_GetObjectItem(choice0, "content") : NULL;
    *out_text = AIRY_STRDUP(cJSON_IsString(content) ? content->valuestring : "");

    if (out_reasoning) {
        cJSON *reasoning = choice0 ? cJSON_GetObjectItem(choice0, "reasoning_content") : NULL;
        if (cJSON_IsString(reasoning) && reasoning->valuestring && reasoning->valuestring[0])
            *out_reasoning = AIRY_STRDUP(reasoning->valuestring);
    }

    cJSON *usage = result ? cJSON_GetObjectItem(result, "usage") : NULL;
    cJSON *total = usage ? cJSON_GetObjectItem(usage, "total_tokens") : NULL;
    if (!cJSON_IsNumber(total))
        total = result ? cJSON_GetObjectItem(result, "total_tokens") : NULL;
    if (!cJSON_IsNumber(total) && cJSON_IsObject(usage)) {
        cJSON *u_pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON *u_ct = cJSON_GetObjectItem(usage, "completion_tokens");
        if (cJSON_IsNumber(u_pt) || cJSON_IsNumber(u_ct)) {
            uint64_t sum = 0;
            if (cJSON_IsNumber(u_pt))
                sum += (uint64_t)(u_pt->valuedouble > 0 ? u_pt->valuedouble : 0);
            if (cJSON_IsNumber(u_ct))
                sum += (uint64_t)(u_ct->valuedouble > 0 ? u_ct->valuedouble : 0);
            *out_tokens = sum;
        }
    } else if (cJSON_IsNumber(total)) {
        *out_tokens = (uint64_t)(total->valuedouble > 0 ? total->valuedouble : 0);
    }
    if (out_cost) {
        cJSON *cost = result ? cJSON_GetObjectItem(result, "cost_usd") : NULL;
        if (cJSON_IsNumber(cost))
            *out_cost = cost->valuedouble;
    }
    cJSON_Delete(root);
    return 0;
}

/* 提取 llm_d 响应的 tool_calls 数组。 */
static int run_parse_tool_calls(const char *llm_resp, cJSON **out_tool_calls)
{
    *out_tool_calls = NULL;
    cJSON *root = cJSON_Parse(llm_resp);
    if (!root)
        return -1;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 =
        (choices && cJSON_GetArraySize(choices) > 0) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *tc = choice0 ? cJSON_GetObjectItem(choice0, "tool_calls") : NULL;
    if (cJSON_IsArray(tc) && cJSON_GetArraySize(tc) > 0)
        *out_tool_calls = cJSON_Duplicate(tc, 1);
    cJSON_Delete(root);
    return *out_tool_calls ? 0 : -1;
}

/* 单个工具执行：tool_d.execute_tool（结果文本化，失败转 Error: 前缀）。 */
static int run_execute_tool(const char *name, const char *args_json, char **out_text)
{
    *out_text = NULL;
    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON_AddStringToObject(params, "tool_id", name);
    cJSON *pargs = cJSON_Parse(args_json && args_json[0] ? args_json : "{}");
    if (!pargs)
        pargs = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "params", pargs);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    char sock[AGENT_RUN_SOCK_BUF];
    snprintf(sock, sizeof(sock), "%s", airy_runtime_dir_socket("tool.sock"));
    char *resp = NULL;
    int rc = daemon_rpc_call(sock, "execute_tool", params_str, &resp, AGENT_RUN_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (rc != AIRY_SUCCESS || !resp) {
        *out_text = AIRY_STRDUP("Tool service unreachable");
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root) {
        *out_text = AIRY_STRDUP("Tool service returned invalid response");
        return -1;
    }
    char *text = NULL;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *err = cJSON_GetObjectItem(root, "error");
    int tool_ok = 0;
    if (result) {
        cJSON *success = cJSON_GetObjectItem(result, "success");
        cJSON *output = cJSON_GetObjectItem(result, "output");
        cJSON *error = cJSON_GetObjectItem(result, "error");
        tool_ok = cJSON_IsNumber(success) && success->valueint != 0;
        if (tool_ok) {
            text = AIRY_STRDUP(cJSON_IsString(output) && output->valuestring ? output->valuestring :
                                                                               "(no output)");
        } else {
            const char *e =
                cJSON_IsString(error) && error->valuestring ? error->valuestring : "execution failed";
            size_t elen = strlen(e) + 8;
            text = (char *)AIRY_MALLOC(elen);
            if (text)
                snprintf(text, elen, "Error: %s", e);
        }
    } else if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        const char *m = cJSON_IsString(msg) && msg->valuestring ? msg->valuestring : "unknown";
        size_t elen = strlen(m) + 8;
        text = (char *)AIRY_MALLOC(elen);
        if (text)
            snprintf(text, elen, "Error: %s", m);
    }
    if (!text)
        text = AIRY_STRDUP("Tool execution returned no result");
    *out_text = text;
    cJSON_Delete(root);
    return tool_ok ? 0 : -1;
}

int agent_run_tool_loop(const char *prompt, const cJSON *history, const char *model,
                        const agent_run_session_t *session, cJSON **out_trace, char **out_text,
                        uint64_t *out_tokens, double *out_cost, char **out_reasoning)
{
    *out_trace = NULL;
    *out_text = NULL;
    *out_tokens = 0;
    if (out_cost)
        *out_cost = 0.0;
    if (out_reasoning)
        *out_reasoning = NULL;

    cJSON *messages = NULL;
    if (history && cJSON_IsArray(history) && cJSON_GetArraySize(history) > 0) {
        messages = cJSON_Duplicate(history, 1);
    }
    if (!messages) {
        messages = cJSON_CreateArray();
        cJSON *msg0 = cJSON_CreateObject();
        cJSON_AddStringToObject(msg0, "role", "user");
        cJSON_AddStringToObject(msg0, "content", prompt);
        cJSON_AddItemToArray(messages, msg0);
    }
    cJSON *tool_trace = cJSON_CreateArray();
    if (!messages || !tool_trace) {
        if (messages)
            cJSON_Delete(messages);
        if (tool_trace)
            cJSON_Delete(tool_trace);
        return -1;
    }

    char *final_text = NULL;
    char *reasoning_acc = NULL;
    uint64_t total_tokens = 0;
    double total_cost = 0.0;
    int rc = -1;

    char llm_sock[AGENT_RUN_SOCK_BUF];
    snprintf(llm_sock, sizeof(llm_sock), "%s", airy_runtime_dir_socket("llm.sock"));

    for (int loops = 0; loops < AGENT_RUN_MAX_TOOL_LOOPS; loops++) {
        if (agent_run_is_cancelled(session)) {
            SVC_LOG_INFO("agent.run: cancelled by user (session=%s)",
                         session ? session->session_id : "?");
            rc = 1;
            break;
        }

        char *llm_params_str = run_build_llm_params(model, messages);
        if (!llm_params_str)
            break;
        char *llm_resp = NULL;
        int lrc = daemon_rpc_call(llm_sock, "complete", llm_params_str, &llm_resp,
                                  AGENT_RUN_LLM_TIMEOUT_MS);
        AIRY_FREE(llm_params_str);
        if (lrc != AIRY_SUCCESS || !llm_resp)
            break;

        cJSON *tool_calls = NULL;
        run_parse_tool_calls(llm_resp, &tool_calls);

        char *text = NULL;
        char *reasoning = NULL;
        uint64_t tokens = 0;
        double cost = 0.0;
        run_parse_result(llm_resp, &text, &tokens, &cost, &reasoning);
        total_tokens += tokens;
        total_cost += cost;
        if (reasoning && reasoning[0]) {
            size_t old = reasoning_acc ? strlen(reasoning_acc) : 0;
            size_t add = strlen(reasoning);
            char *np = (char *)AIRY_REALLOC(reasoning_acc, old + add + 2);
            if (np) {
                reasoning_acc = np;
                if (old > 0)
                    reasoning_acc[old++] = '\n';
                AIRY_MEMCPY(reasoning_acc + old, reasoning, add);
                reasoning_acc[old + add] = '\0';
            }
        }
        AIRY_FREE(reasoning);

        cJSON *assistant_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(assistant_msg, "role", "assistant");
        cJSON_AddStringToObject(assistant_msg, "content", text ? text : "");
        if (tool_calls)
            cJSON_AddItemToObject(assistant_msg, "tool_calls", cJSON_Duplicate(tool_calls, 1));
        cJSON_AddItemToArray(messages, assistant_msg);

        if (!tool_calls) {
            final_text = text ? text : AIRY_STRDUP("");
            AIRY_FREE(llm_resp);
            rc = 0;
            break;
        }
        AIRY_FREE(text);

        int tc_count = cJSON_GetArraySize(tool_calls);
        for (int i = 0; i < tc_count; i++) {
            cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *fn_name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            cJSON *fn_args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
            cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
            const char *tname = cJSON_IsString(fn_name) ? fn_name->valuestring : "";
            const char *targs = cJSON_IsString(fn_args) ? fn_args->valuestring : "{}";
            const char *tid = cJSON_IsString(tc_id) ? tc_id->valuestring : "";

            char *result_text = NULL;
            int erc = run_execute_tool(tname, targs, &result_text);

            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", tid);
            cJSON_AddStringToObject(tool_msg, "content",
                                    result_text ? result_text : "Tool execution failed");
            cJSON_AddItemToArray(messages, tool_msg);

            cJSON *tr = cJSON_CreateObject();
            cJSON_AddStringToObject(tr, "tool", tname);
            cJSON_AddStringToObject(tr, "arguments", targs);
            cJSON_AddStringToObject(tr, "result", result_text ? result_text : "");
            cJSON_AddNumberToObject(tr, "ok", erc == 0 ? 1 : 0);
            cJSON_AddItemToArray(tool_trace, tr);

            if (result_text)
                AIRY_FREE(result_text);
        }

        cJSON_Delete(tool_calls);
        AIRY_FREE(llm_resp);
    }

    cJSON_Delete(messages);

    if (rc == 0) {
        *out_trace = tool_trace;
        *out_text = final_text;
        *out_tokens = total_tokens;
        if (out_cost)
            *out_cost = total_cost;
        if (out_reasoning) {
            *out_reasoning = reasoning_acc;
            reasoning_acc = NULL;
        }
    } else {
        if (final_text)
            AIRY_FREE(final_text);
        cJSON_Delete(tool_trace);
    }
    AIRY_FREE(reasoning_acc);
    return rc;
}
