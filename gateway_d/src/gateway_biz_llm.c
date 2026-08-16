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

/* Builtin tool OpenAI tools schema (SSoT): one-to-one with the tools
 * registered in tool_d builtin.c/service.c. All "required" fields must
 * match the parameter sets registered in tool_d: its validator treats
 * every registered parameter as mandatory, so if the schema marks one
 * optional while tool_d requires it (e.g. fs_list's path), the LLM may
 * omit it and tool validation fails. */
#include "../../../gateway/src/gateway/gateway_tools_schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#ifdef _WIN32
static int llm_connect_tcp(const gateway_business_ctx_t *ctx)
{
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
        return -1;
    struct sockaddr_in addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx->llm_tcp_port);
    inet_pton(AF_INET, ctx->llm_tcp_addr, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(fd);
        return -1;
    }
    return (int)fd;
}
#else
static int llm_connect_unix(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif

/**
 * @brief Send a JSON-RPC complete request to llm_d and read the response
 * @return Response string (AIRY_MALLOC), or NULL on failure
 */
static char *llm_call_complete(const gateway_business_ctx_t *ctx, const char *req_json)
{
    int fd;
#ifdef _WIN32
    fd = llm_connect_tcp(ctx);
#else
    fd = llm_connect_unix(ctx->llm_sock_path);
#endif
    if (fd < 0) {
        AIRY_LOG_WARN("gateway handler: cannot connect to llm_d (sock=%s)", ctx->llm_sock_path);
        return NULL;
    }

#ifdef _WIN32
    int timeout_ms = GW_LLM_DEFAULT_TIMEOUT_MS;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv = {GW_LLM_DEFAULT_TIMEOUT_MS / 1000,
                         (GW_LLM_DEFAULT_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    size_t len = strlen(req_json);
    size_t sent_total = 0;
    while (sent_total < len) {
#ifdef _WIN32
        int n = send(fd, req_json + sent_total, (int)(len - sent_total), 0);
#else
        ssize_t n = send(fd, req_json + sent_total, len - sent_total, 0);
#endif
        if (n <= 0) {
#ifdef _WIN32
            closesocket(fd);
#else
            close(fd);
#endif
            return NULL;
        }
        sent_total += (size_t)n;
    }

    size_t cap = 4096;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        return NULL;
    }
    resp[0] = '\0';

    char buf[4096];
    for (;;) {
#ifdef _WIN32
        int n = recv(fd, buf, sizeof(buf), 0);
#else
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
#endif
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > GW_LLM_MAX_RESP) {
                AIRY_FREE(resp);
#ifdef _WIN32
                closesocket(fd);
#else
                close(fd);
#endif
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
#ifdef _WIN32
                closesocket(fd);
#else
                close(fd);
#endif
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }

#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    return resp;
}

static const char GW_TOOLS_JSON[] = GW_TOOLS_JSON_SOURCE;

/**
 * @brief Send a JSON-RPC request to tool_d and read the response (POSIX Unix socket)
 * @return Response string (AIRY_MALLOC), or NULL on failure
 */
static char *tool_call_rpc(const gateway_business_ctx_t *ctx, const char *req_json)
{
#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, ctx->tool_sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        AIRY_LOG_WARN("gateway handler: cannot connect to tool_d (sock=%s)", ctx->tool_sock_path);
        close(fd);
        return NULL;
    }

    struct timeval tv = {GW_TOOL_TIMEOUT_MS / 1000, (GW_TOOL_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t len = strlen(req_json);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_json + sent, len - sent, 0);
        if (n <= 0) {
            close(fd);
            return NULL;
        }
        sent += (size_t)n;
    }

    size_t cap = 65536;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
        close(fd);
        return NULL;
    }
    resp[0] = '\0';
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > GW_LLM_MAX_RESP) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }
    close(fd);
    return resp;
#else
    (void)ctx;
    (void)req_json;
    return NULL;
#endif
}

/**
 * @brief Extract reply text and token usage from the llm response
 * @return 0 on success (*out_text / *out_tokens / *out_cost valid), non-zero on failure
 */
static int parse_llm_result(const char *llm_resp, char **out_text, uint64_t *out_tokens,
                            double *out_cost)
{
    *out_text = NULL;
    *out_tokens = 0;
    if (out_cost)
        *out_cost = 0.0;

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

    if (gw_acl_check_tool(name) != 0) {
        *out_text = AIRY_STRDUP("Permission denied: tool not authorized");
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    if (!req)
        return -1;
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", "execute_tool");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "tool_id", name);
    cJSON *pargs = cJSON_Parse(args_json && args_json[0] ? args_json : "{}");
    if (!pargs)
        pargs = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "params", pargs);
    cJSON_AddItemToObject(req, "params", params);
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str)
        return -1;

    char *resp = tool_call_rpc(ctx, req_str);
    AIRY_FREE(req_str);
    if (!resp) {
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
 * @brief Build the llm_d complete JSON-RPC request (passes through the tools array)
 * @param model    Model name
 * @param messages Conversation history (cJSON array, deep-copied into the request)
 * @return JSON request string (AIRY_MALLOC), or NULL on failure
 */
static char *gw_build_llm_request(const char *model, const cJSON *messages)
{
    cJSON *llm_req = cJSON_CreateObject();
    if (!llm_req)
        return NULL;
    cJSON_AddStringToObject(llm_req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(llm_req, "id", 1);
    cJSON_AddStringToObject(llm_req, "method", "complete");
    cJSON *llm_params = cJSON_CreateObject();
    if (!llm_params) {
        cJSON_Delete(llm_req);
        return NULL;
    }
    cJSON_AddStringToObject(llm_params, "model", model);
    cJSON_AddItemToObject(llm_params, "messages", cJSON_Duplicate(messages, 1));
    cJSON *tools = cJSON_Parse(GW_TOOLS_JSON);
    if (tools) {
        cJSON_AddItemToObject(llm_params, "tools", tools);
    }
    cJSON_AddNumberToObject(llm_params, "max_tokens", 2048);
    cJSON_AddNumberToObject(llm_params, "temperature", 0.7);
    cJSON_AddItemToObject(llm_req, "params", llm_params);

    char *req_str = cJSON_PrintUnformatted(llm_req);
    cJSON_Delete(llm_req);
    return req_str;
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
                     char **out_text, uint64_t *out_tokens, double *out_cost)
{
    *out_trace = NULL;
    *out_text = NULL;
    *out_tokens = 0;
    if (out_cost)
        *out_cost = 0.0;

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
    int rc = -1;

    for (int loops = 0; loops < GW_MAX_TOOL_LOOPS; loops++) {

        if (gw_active_is_cancelled(active)) {
            AIRY_LOG_INFO("gateway: agent.run cancelled by user (session=%s)",
                     active ? active->session_id : "?");
            rc = 1;
            break;
        }

        char *llm_req_str = gw_build_llm_request(model, messages);
        if (!llm_req_str) {
            break;
        }
        char *llm_resp = llm_call_complete(ctx, llm_req_str);
        AIRY_FREE(llm_req_str);
        if (!llm_resp) {
            break;
        }

        cJSON *tool_calls = NULL;
        parse_llm_tool_calls(llm_resp, &tool_calls);

        char *text = NULL;
        uint64_t tokens = 0;
        double cost = 0.0;
        parse_llm_result(llm_resp, &text, &tokens, &cost);
        total_tokens += tokens;
        total_cost += cost;

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
    } else {
        if (final_text)
            AIRY_FREE(final_text);
        cJSON_Delete(tool_trace);
    }
    return rc;
}
