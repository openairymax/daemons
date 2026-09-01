// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_llm.c
 * @brief Gateway LLM-call layer and agent tool loop (ReAct).
 *
 * Wraps the llm_d direct connection (complete) and tool_d execution
 * (execute_tool), implementing the ReAct tool loop
 * LLM -> tool_calls -> execute -> feed back. Dual-think products (GCCP+GRAD
 * DAG plans) are injected into messages by the upper layer and executed
 * through this loop.
 *
 * Split from gateway_business_handler.c (single responsibility: LLM/tool
 * execution).
 */

#include "gateway_biz_internal.h"

#include "logging.h"
#include "platform.h"

#include "syscalls.h"

/* Builtin tool OpenAI tools schema (SSoT): one-to-one with the tools
 * registered in tool_d builtin.c/service.c. All "required" fields must
 * match the parameter sets registered in tool_d: its validator treats
 * every registered parameter as mandatory, so if the schema marks one
 * optional while tool_d requires it (e.g. fs_list's path), the LLM may
 * omit it and tool validation fails. */
#include "../../../commons/include/airy_tool_schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char GW_TOOLS_JSON[] = AIRY_TOOLS_JSON_SOURCE;

/**
 * @brief Build the llm_d complete JSON-RPC request (passes through the tools array)
 * @param model    Model name
 * @param messages Conversation history (cJSON array, deep-copied into the request)
 * @return JSON request string (AIRY_MALLOC), or NULL on failure
 */
static char *gw_build_llm_params(const char *model, const cJSON *messages)
{
    cJSON *llm_params = cJSON_CreateObject();
    if (!llm_params)
        return NULL;
    cJSON_AddStringToObject(llm_params, "model", model);
    cJSON_AddItemToObject(llm_params, "messages", cJSON_Duplicate(messages, 1));
    cJSON *tools = cJSON_Parse(GW_TOOLS_JSON);
    if (tools) {
        cJSON_AddItemToObject(llm_params, "tools", tools);
    }
    cJSON_AddNumberToObject(llm_params, "max_tokens", 2048);
    cJSON_AddNumberToObject(llm_params, "temperature", 0.7);

    char *params_str = cJSON_PrintUnformatted(llm_params);
    cJSON_Delete(llm_params);
    return params_str;
}

/**
 * @brief Extract reply text, reasoning and token usage from the llm response
 * @return 0 on success (*out_text / *out_tokens / *out_cost valid), non-zero on failure
 */
static int parse_llm_result(const char *llm_resp, char **out_text, uint64_t *out_tokens,
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
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        AIRY_LOG_WARN("gateway handler: llm_d error: %s", cJSON_IsString(msg) ? msg->valuestring : "?");
        cJSON_Delete(root);
        return -1;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 =
        (choices && cJSON_GetArraySize(choices) > 0) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *content = choice0 ? cJSON_GetObjectItem(choice0, "content") : NULL;
    if (cJSON_IsString(content)) {
        *out_text = AIRY_STRDUP(content->valuestring);
    } else {
        /* In tool-call rounds the LLM content is usually null (response holds
         * only tool_calls); fall back to an empty string instead of failing so
         * the tool loop can continue. */
        *out_text = AIRY_STRDUP("");
    }

    /* 2.1.1.6：非流式路径保留思考链（llm_d 已在 choices[0] 透传
     * reasoning_content，gateway 此前直接丢弃，agent.run 结果不含思考
     * token）。 */
    if (out_reasoning) {
        cJSON *reasoning = choice0 ? cJSON_GetObjectItem(choice0, "reasoning_content") : NULL;
        if (cJSON_IsString(reasoning) && reasoning->valuestring && reasoning->valuestring[0])
            *out_reasoning = AIRY_STRDUP(reasoning->valuestring);
    }

    /* tokens: prefer usage.total_tokens (OpenAI compatible), then top-level
     * total_tokens (llm_d legacy format); if both are missing, sum
     * prompt+completion inside usage. */
    cJSON *usage = result ? cJSON_GetObjectItem(result, "usage") : NULL;
    cJSON *total = usage ? cJSON_GetObjectItem(usage, "total_tokens") : NULL;
    if (!cJSON_IsNumber(total)) {
        total = result ? cJSON_GetObjectItem(result, "total_tokens") : NULL;
    }
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

/**
 * @brief Extract tool_calls from the llm_d response (choices[0].tool_calls)
 * @return 0 with tool_calls present (*out caller cJSON_Delete), non-zero without
 */
static int parse_llm_tool_calls(const char *llm_resp, cJSON **out_tool_calls)
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
    if (cJSON_IsArray(tc) && cJSON_GetArraySize(tc) > 0) {
        *out_tool_calls = cJSON_Duplicate(tc, 1);
    }
    cJSON_Delete(root);
    return *out_tool_calls ? 0 : -1;
}

/**
 * @brief Execute a single tool via tool_d execute_tool
 * @param name      Tool name (fs_read/fs_write/fs_list/shell_run)
 * @param args_json Tool args (OpenAI tool_call arguments JSON string)
 * @param out_text  Result text (AIRY_MALLOC, caller frees); error description on failure
 * @return 0 on success, non-zero on failure
 */
static int gw_execute_tool(const gateway_business_ctx_t *ctx, const char *name,
                           const char *args_json, char **out_text)
{
    *out_text = NULL;
    (void)ctx; /* 端点解析统一由 svc dispatch 钩子按命名空间完成 */

    if (gw_acl_check_tool(name) != 0) {
        *out_text = AIRY_STRDUP("Permission denied: tool not authorized");
        return -1;
    }

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

    /* 架构约束 2026-08-25 "必须走 syscall": tool.execute_tool 经 SYS_SVC_CALL 派发 */
    char *resp = NULL;
    airy_err_t rc = airy_sys_svc_call("tool", "execute_tool", params_str, GW_TOOL_TIMEOUT_MS,
                                      &resp);
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

    /* Build result text: prefer output; on error "Error: <error>".
     * Return code semantics: 0 = tool executed successfully, non-zero = tool
     * layer failure (RPC succeeded but the tool errored/raised). */
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
            const char *e = cJSON_IsString(error) && error->valuestring ? error->valuestring :
                                                                          "execution failed";
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

/**
 * @brief Run the agent tool loop: LLM -> tool_calls -> execute -> feed back ->
 *        continue (ReAct)
 *
 * The full reasoning + tool chain of a single agent.run. The loop is capped at
 * GW_MAX_TOOL_LOOPS rounds to avoid runaway; after each round the assistant
 * (with tool_calls) and tool (with tool_call_id) messages are appended to the
 * conversation history for the next LLM round.
 *
 * @param ctx       Gateway context
 * @param model     Model name
 * @param prompt    User input
 * @param history   Full conversation history (OpenAI messages array, may be
 *        NULL). When non-empty it seeds the tool loop (true multi-turn context
 *        across requests, M2 fix) with the last user message as the current
 *        input; empty/invalid history degrades to a single prompt message.
 * @param active    In-flight request entry (agent.cancel support; the cancel
 *        flag is checked between rounds)
 * @param out_trace Tool trace array (cJSON, caller cJSON_Delete; NULL on failure)
 * @param out_text  Final reply text (AIRY_MALLOC, caller AIRY_FREE; NULL on failure)
 * @param out_tokens Cumulative token usage
 * @param out_cost  Cumulative cost (USD)
 * @return 0 success (*out_text valid); 1 user cancelled; non-zero failure
 *         (no final answer obtained)
 */
int gw_run_tool_loop(const gateway_business_ctx_t *ctx, const char *model, const char *prompt,
                     const cJSON *history, gw_active_request_t *active, cJSON **out_trace,
                     char **out_text, uint64_t *out_tokens, double *out_cost,
                     char **out_reasoning)
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
    uint64_t total_tokens = 0;
    double total_cost = 0.0;
    char *reasoning_acc = NULL; /* 2.1.1.6：多轮思考链拼接（换行分隔） */
    int rc = -1;

    for (int loops = 0; loops < GW_MAX_TOOL_LOOPS; loops++) {

        if (gw_active_is_cancelled(active)) {
            AIRY_LOG_INFO("gateway: agent.run cancelled by user (session=%s)",
                     active ? active->session_id : "?");
            rc = 1;
            break;
        }

        char *llm_params_str = gw_build_llm_params(model, messages);
        if (!llm_params_str) {
            break;
        }
        /* 架构约束 2026-08-25 "必须走 syscall": llm.complete 经 SYS_SVC_CALL 派发 */
        char *llm_resp = NULL;
        airy_err_t lrc = airy_sys_svc_call("llm", "complete", llm_params_str,
                                           GW_LLM_DEFAULT_TIMEOUT_MS, &llm_resp);
        AIRY_FREE(llm_params_str);
        if (lrc != AIRY_SUCCESS || !llm_resp) {
            break;
        }

        cJSON *tool_calls = NULL;
        parse_llm_tool_calls(llm_resp, &tool_calls);

        char *text = NULL;
        uint64_t tokens = 0;
        double cost = 0.0;
        char *reasoning = NULL;
        parse_llm_result(llm_resp, &text, &tokens, &cost, &reasoning);
        total_tokens += tokens;
        total_cost += cost;
        /* 2.1.1.6：累积思考链（多轮工具循环时每轮各自产生 reasoning） */
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
        if (tool_calls) {
            cJSON_AddItemToObject(assistant_msg, "tool_calls", cJSON_Duplicate(tool_calls, 1));
        }
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
            int erc = gw_execute_tool(ctx, tname, targs, &result_text);

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
        if (out_reasoning)
            *out_reasoning = NULL; /* 失败时已释放，避免调用方双重释放 */
    }
    AIRY_FREE(reasoning_acc);
    return rc;
}
