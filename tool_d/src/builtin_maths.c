// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_maths.c
 * @brief Built-in tool maths domain: maths_eval / maths_stats.
 *
 * 接线：tool_d 内置工具经 Unix Socket（maths.sock）调用 maths_d 数学外挂
 * 计算服务（JSON-RPC 2.0 over unix socket）。把数学表达式求值从 LLM 推理
 * 中剥离（Tri-Opt 3.3 "外挂计算器"）。
 *
 * 安全边界：仅转发数值表达式/统计参数到 maths_d 沙箱求值器（纯数值计算，
 * 无代码执行、无网络）；maths.sock 仅本机监听。
 *
 * Windows 暂不启用（maths_d 在 Windows 回退 TCP 监听，未列入内置工具），
 * 与 git_* 工具同策略。
 */

#include "airy_memory.h"
#include "error.h"

#include "builtin.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <cjson_helpers.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "tool_builtin_internal.h"

#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define MATHS_SOCKET_NAME "maths.sock"
#define MATHS_RPC_TIMEOUT_MS 3000
#define MATHS_RESP_CAP 8192

#ifndef _WIN32

/* 经 Unix Socket 向 maths_d 发送一条 JSON-RPC 请求并收取响应。
 * 返回 0 成功（resp 含完整响应体）；负值失败（res->error 已置）。 */
static int maths_rpc_call(const char *method, const char *params_json,
                          char *resp, size_t resp_sz, tool_result_t *res)
{
    if (!method || !params_json || !resp || resp_sz == 0)
        return AIRY_ERR_INVALID_PARAM;

    const char *sock_path = airy_runtime_dir_socket(MATHS_SOCKET_NAME);
    if (!sock_path || !sock_path[0]) {
        res->error = AIRY_STRDUP("maths_d socket path unavailable");
        return AIRY_ERR_STATE_ERROR;
    }

    int fd = (int)socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        res->error = AIRY_STRDUP("maths_d: socket() failed");
        return AIRY_ERR_EXEC_FAIL;
    }

    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        res->error = AIRY_STRDUP("maths_d socket path too long");
        close(fd);
        return AIRY_ERR_INVALID_PARAM;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        char err[256];
        snprintf(err, sizeof(err),
                 "maths_d not reachable at %s (is maths_d running?)", sock_path);
        res->error = AIRY_STRDUP(err);
        close(fd);
        return AIRY_ERR_STATE_ERROR;
    }

    char req[8192];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}",
             method, params_json);

    if (send(fd, req, strlen(req), MSG_NOSIGNAL) != (ssize_t)strlen(req)) {
        res->error = AIRY_STRDUP("maths_d: send() failed");
        close(fd);
        return AIRY_ERR_EXEC_FAIL;
    }

    /* 等待首包（防 maths_d 卡死阻塞 tool_d），再循环 recv 收完整响应 */
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, MATHS_RPC_TIMEOUT_MS);
    if (pr <= 0 || !(pfd.revents & POLLIN)) {
        res->error = AIRY_STRDUP("maths_d: response timeout");
        close(fd);
        return AIRY_ERR_TIMEOUT;
    }

    size_t total = 0;
    for (;;) {
        ssize_t n = recv(fd, resp + total, resp_sz - total - 1, 0);
        if (n <= 0)
            break;
        total += (size_t)n;
        if (total >= resp_sz - 1)
            break;
    }
    resp[total] = '\0';
    close(fd);

    if (total == 0) {
        res->error = AIRY_STRDUP("maths_d: empty response");
        return AIRY_ERR_EXEC_FAIL;
    }
    return 0;
}

/* 解析 maths_d JSON-RPC 响应：成功返回 result 对象（cJSON，调用方
 * cJSON_Delete）；失败置 res->error 并返回 NULL。 */
static cJSON *maths_parse_response(const char *resp, tool_result_t *res)
{
    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        res->error = AIRY_STRDUP("maths_d: malformed response JSON");
        return NULL;
    }
    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsObject(err)) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        const char *text = (cJSON_IsString(msg) && msg->valuestring)
                               ? msg->valuestring
                               : "maths_d error";
        char out[512];
        snprintf(out, sizeof(out), "maths error: %s", text);
        res->error = AIRY_STRDUP(out);
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsObject(result)) {
        res->error = AIRY_STRDUP("maths_d: response missing result");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}
#endif /* _WIN32 */

/**
 * @brief maths.eval — 数学表达式求值（委托 maths_d）
 * 参数：{"expression":"sqrt(144)+2"}  或  {"expr":"..."}
 */
int maths_eval_tool(const char *params_json, tool_result_t *res)
{
    if (!res)
        return AIRY_ERR_INVALID_PARAM;
#ifdef _WIN32
    res->error = AIRY_STRDUP("maths.eval is not supported on Windows");
    return AIRY_ERR_NOT_SUPPORTED;
#else
    /* CJSON_PARSE_GUARD 使用 CJSON_AUTO_FREE（作用域结束时自动释放），
     * 因此本函数内不得再显式 cJSON_Delete(root)。 */
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });

    cJSON *expr = cJSON_GetObjectItem(root, "expression");
    if (!cJSON_IsString(expr))
        expr = cJSON_GetObjectItem(root, "expr");
    if (!cJSON_IsString(expr) || !expr->valuestring || !expr->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: expression");
        return AIRY_ERR_INVALID_PARAM;
    }
    if (strlen(expr->valuestring) > 4096) {
        res->error = AIRY_STRDUP("expression too long (max 4096 chars)");
        return AIRY_ERR_INVALID_PARAM;
    }

    char params[8192];
    /* cJSON 序列化 expression，保证 JSON 转义正确 */
    cJSON *params_obj = cJSON_CreateObject();
    if (!params_obj) {
        res->error = AIRY_STRDUP("OOM");
        return AIRY_ERR_GENERIC_FAIL;
    }
    cJSON_AddItemToObject(params_obj, "expr",
                          cJSON_CreateString(expr->valuestring));
    char *payload = cJSON_PrintUnformatted(params_obj);
    cJSON_Delete(params_obj);
    if (!payload) {
        res->error = AIRY_STRDUP("OOM");
        return AIRY_ERR_GENERIC_FAIL;
    }
    if (strlen(payload) + 32 >= sizeof(params)) {
        cJSON_free(payload);
        res->error = AIRY_STRDUP("request too large");
        return AIRY_ERR_INVALID_PARAM;
    }
    snprintf(params, sizeof(params), "%s", payload);
    cJSON_free(payload);

    char resp[MATHS_RESP_CAP];
    int rc = maths_rpc_call("eval", params, resp, sizeof(resp), res);
    if (rc != 0)
        return rc;

    cJSON *rroot = maths_parse_response(resp, res);
    if (!rroot)
        return AIRY_ERR_EXEC_FAIL;
    cJSON *result = cJSON_GetObjectItem(rroot, "result");
    cJSON *value = cJSON_GetObjectItem(result, "result");
    if (!cJSON_IsNumber(value)) {
        cJSON_Delete(rroot);
        res->error = AIRY_STRDUP("maths.eval: unexpected response shape");
        return AIRY_ERR_EXEC_FAIL;
    }
    char out[64];
    snprintf(out, sizeof(out), "%.12g", value->valuedouble);
    res->output = AIRY_STRDUP(out);
    res->exit_code = 0;
    res->success = 1;
    cJSON_Delete(rroot);
    return 0;
#endif
}

/**
 * @brief maths.stats — 描述性统计（委托 maths_d）
 * 参数：{"op":"mean|median|variance|stddev|sum|min|max","values":[1,2,3]}
 */
int maths_stats_tool(const char *params_json, tool_result_t *res)
{
    if (!res)
        return AIRY_ERR_INVALID_PARAM;
#ifdef _WIN32
    res->error = AIRY_STRDUP("maths.stats is not supported on Windows");
    return AIRY_ERR_NOT_SUPPORTED;
#else
    /* CJSON_PARSE_GUARD 使用 CJSON_AUTO_FREE（作用域结束时自动释放），
     * 因此本函数内不得再显式 cJSON_Delete(root)。 */
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });

    cJSON *op = cJSON_GetObjectItem(root, "op");
    cJSON *values = cJSON_GetObjectItem(root, "values");
    if (!cJSON_IsString(op) || !op->valuestring || !op->valuestring[0] ||
        !cJSON_IsArray(values) || cJSON_GetArraySize(values) == 0) {
        res->error = AIRY_STRDUP("Missing params: op (string) + values (non-empty array)");
        return AIRY_ERR_INVALID_PARAM;
    }

    cJSON *params_obj = cJSON_CreateObject();
    if (!params_obj) {
        res->error = AIRY_STRDUP("OOM");
        return AIRY_ERR_GENERIC_FAIL;
    }
    cJSON_AddItemToObject(params_obj, "op", cJSON_CreateString(op->valuestring));
    cJSON *vals_arr = cJSON_CreateArray();
    if (!vals_arr) {
        cJSON_Delete(params_obj);
        res->error = AIRY_STRDUP("OOM");
        return AIRY_ERR_GENERIC_FAIL;
    }
    cJSON_AddItemToObject(params_obj, "values", vals_arr);
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, values) {
        if (cJSON_IsNumber(item))
            cJSON_AddItemToArray(vals_arr, cJSON_CreateNumber(item->valuedouble));
    }
    if (cJSON_GetArraySize(vals_arr) == 0) {
        cJSON_Delete(params_obj);
        res->error = AIRY_STRDUP("values[] must contain numbers");
        return AIRY_ERR_INVALID_PARAM;
    }
    char *payload = cJSON_PrintUnformatted(params_obj);
    cJSON_Delete(params_obj);
    if (!payload) {
        res->error = AIRY_STRDUP("OOM");
        return AIRY_ERR_GENERIC_FAIL;
    }

    char params[8192];
    if (strlen(payload) + 32 >= sizeof(params)) {
        cJSON_free(payload);
        res->error = AIRY_STRDUP("request too large");
        return AIRY_ERR_INVALID_PARAM;
    }
    snprintf(params, sizeof(params), "%s", payload);
    cJSON_free(payload);

    char resp[MATHS_RESP_CAP];
    int rc = maths_rpc_call("stats", params, resp, sizeof(resp), res);
    if (rc != 0)
        return rc;

    cJSON *rroot = maths_parse_response(resp, res);
    if (!rroot)
        return AIRY_ERR_EXEC_FAIL;
    cJSON *result = cJSON_GetObjectItem(rroot, "result");
    cJSON *value = cJSON_GetObjectItem(result, "result");
    if (!cJSON_IsNumber(value)) {
        cJSON_Delete(rroot);
        res->error = AIRY_STRDUP("maths.stats: unexpected response shape");
        return AIRY_ERR_EXEC_FAIL;
    }
    char out[64];
    snprintf(out, sizeof(out), "%.12g", value->valuedouble);
    res->output = AIRY_STRDUP(out);
    res->exit_code = 0;
    res->success = 1;
    cJSON_Delete(rroot);
    return 0;
#endif
}
