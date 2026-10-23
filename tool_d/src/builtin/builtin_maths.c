// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_maths.c
 * @brief Built-in tool maths domain: maths_eval / maths_stats / maths_plot.
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

#include <stdarg.h>

#include "platform.h"
#include "builtin/builtin_maths_internal.h"

#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define MATHS_RPC_TIMEOUT_MS 3000
#define MATHS_RESP_CAP 8192

#ifndef _WIN32

/* 经 Unix Socket 向 maths_d 发送一条 JSON-RPC 请求并收取响应。
 * 返回 0 成功（resp 含完整响应体）；负值失败（res->error 已置）。 */
static int maths_rpc_call(const char *method, const char *params_json,
                          char *resp, size_t resp_sz, tool_result_t *res)
{
    if (!method || !params_json || !resp || resp_sz == 0) return AIRY_ERR_INVALID_PARAM;

    const char *sock_path = airy_runtime_dir_socket(MATHS_D_SOCKET_NAME);
    if (!sock_path || !sock_path[0])
        return btool_fail(res, AIRY_ERR_STATE_ERROR,
                          "maths_d socket path unavailable");

    int fd = (int)socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return btool_fail(res, AIRY_ERR_EXEC_FAIL, "maths_d: socket() failed");

    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        close(fd);
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "maths_d socket path too long");
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return btool_fail(res, AIRY_ERR_STATE_ERROR,
                          "maths_d not reachable at %s (is maths_d running?)",
                          sock_path);
    }

    char req[8192];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}",
             method, params_json);

    if (send(fd, req, strlen(req), MSG_NOSIGNAL) != (ssize_t)strlen(req)) {
        close(fd);
        return btool_fail(res, AIRY_ERR_EXEC_FAIL, "maths_d: send() failed");
    }

    /* 等待首包（防 maths_d 卡死阻塞 tool_d），再循环 recv 收完整响应 */
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, MATHS_RPC_TIMEOUT_MS);
    if (pr <= 0 || !(pfd.revents & POLLIN)) {
        close(fd);
        return btool_fail(res, AIRY_ERR_TIMEOUT, "maths_d: response timeout");
    }

    size_t total = 0;
    for (;;) {
        ssize_t n = recv(fd, resp + total, resp_sz - total - 1, 0);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= resp_sz - 1) break;
    }
    resp[total] = '\0';
    close(fd);

    if (total == 0) return btool_fail(res, AIRY_ERR_EXEC_FAIL, "maths_d: empty response");
    return 0;
}

/* 解析 maths_d JSON-RPC 响应：成功返回 result 对象（cJSON，调用方
 * cJSON_Delete）；失败置 res->error 并返回 NULL。 */
static cJSON *maths_parse_response(const char *resp, tool_result_t *res)
{
    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        btool_fail(res, AIRY_ERR_EXEC_FAIL, "maths_d: malformed response JSON");
        return NULL;
    }
    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsObject(err)) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        const char *text = (cJSON_IsString(msg) && msg->valuestring)
                               ? msg->valuestring
                               : "maths_d error";
        btool_fail(res, AIRY_ERR_EXEC_FAIL, "maths error: %s", text);
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsObject(result)) {
        btool_fail(res, AIRY_ERR_EXEC_FAIL,
                   "maths_d: response missing result");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* plot fenced 块追加分片：off 为当前写入偏移，溢出返回 -1（调用方
 * 整体降级为错误）。 */
static int maths_plot_append(char *buf, size_t cap, int off, const char *fmt,
                             ...)
{
    if (off < 0 || (size_t)off >= cap) return -1;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + off, cap - (size_t)off, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= cap - (size_t)off) return -1;
    return off + w;
}

/* 请求发送出口：params_obj 序列化为紧凑 JSON，经 maths_rpc_call 发送并
 * 收取响应到 resp；params_obj 由本出口释放。返回 rpc 调用码（失败已置
 * res->error）。 */
static int maths_send(const char *method, cJSON *params_obj, char *resp,
                      size_t resp_sz, tool_result_t *res)
{
    char *payload = cJSON_PrintUnformatted(params_obj);
    cJSON_Delete(params_obj);
    if (!payload) return btool_fail(res, AIRY_ERR_GENERIC_FAIL, "OOM");
    char params[8192];
    if (strlen(payload) + 32 >= sizeof(params)) {
        cJSON_free(payload);
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "request too large");
    }
    snprintf(params, sizeof(params), "%s", payload);
    cJSON_free(payload);
    return maths_rpc_call(method, params, resp, resp_sz, res);
}

/* 标量结果出口：解析响应并取 result.result 数值格式化填 res（who 作
 * 错误前缀）；响应体由本出口统一释放。 */
static int maths_scalar(const char *resp, const char *who, tool_result_t *res)
{
    cJSON *rroot = maths_parse_response(resp, res);
    if (!rroot) return AIRY_ERR_EXEC_FAIL;
    cJSON *value = cJSON_GetObjectItem(cJSON_GetObjectItem(rroot, "result"),
                                       "result");
    if (!cJSON_IsNumber(value)) {
        cJSON_Delete(rroot);
        return btool_fail(res, AIRY_ERR_EXEC_FAIL,
                          "%s: unexpected response shape", who);
    }
    char out[64];
    snprintf(out, sizeof(out), "%.12g", value->valuedouble);
    cJSON_Delete(rroot);
    return btool_okout(res, AIRY_STRDUP(out));
}

/* plot 参数校验：表达式（expression 或 expr）+ 数值型 xmin/xmax 必需，
 * 且长度与区间须合法。通过则回填三个出参并返回 AIRY_OK。 */
static int plot_check(const cJSON *root, cJSON **expr, cJSON **xmin, cJSON **xmax,
                      tool_result_t *res)
{
    *expr = cJSON_GetObjectItem(root, "expression");
    if (!cJSON_IsString(*expr))
        *expr = cJSON_GetObjectItem(root, "expr");
    *xmin = cJSON_GetObjectItem(root, "xmin");
    *xmax = cJSON_GetObjectItem(root, "xmax");
    if (!cJSON_IsString(*expr) || !(*expr)->valuestring || !(*expr)->valuestring[0] ||
        !cJSON_IsNumber(*xmin) || !cJSON_IsNumber(*xmax))
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "Missing params: expression (string) + xmin/xmax "
                          "(number)");
    if (strlen((*expr)->valuestring) > 4096)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "expression too long (max 4096 chars)");
    if (!((*xmin)->valuedouble < (*xmax)->valuedouble))
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "xmin must be strictly less than xmax");
    return AIRY_OK;
}

/* plot 请求参数对象：expr/xmin/xmax 必备，samples 可选。返回 NULL 表示
 * OOM，调用方降级为错误。 */
static cJSON *plot_params(const cJSON *expr, const cJSON *xmin, const cJSON *xmax,
                          const cJSON *root)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddItemToObject(obj, "expr", cJSON_CreateString(expr->valuestring));
    cJSON_AddItemToObject(obj, "xmin", cJSON_CreateNumber(xmin->valuedouble));
    cJSON_AddItemToObject(obj, "xmax", cJSON_CreateNumber(xmax->valuedouble));
    cJSON *samples = cJSON_GetObjectItem(root, "samples");
    if (cJSON_IsNumber(samples))
        cJSON_AddItemToObject(obj, "samples",
                              cJSON_CreateNumber(samples->valuedouble));
    return obj;
}

/* plot 单序列追加：null_ok 允许 null 元素（ys 断点）写成 null 字面量。
 * 溢出返回 -1，后续追加自动短路。 */
static int plot_series(char *out, size_t cap, int off, const cJSON *arr, int null_ok)
{
    int first = 1;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (off < 0) break;
        const char *sep = first ? "" : ",";
        if (null_ok && cJSON_IsNull(it))
            off = maths_plot_append(out, cap, off, "%snull", sep);
        else
            off = maths_plot_append(out, cap, off, "%s%.6g", sep, it->valuedouble);
        first = 0;
    }
    return off;
}

/* plot 响应出口：校验 xs/ys 形状后组装 ```plot fenced 块并交付；响应体
 * 由本出口统一释放。 */
static int plot_emit(const char *expr_str, cJSON *rroot, tool_result_t *res)
{
    cJSON *result = cJSON_GetObjectItem(rroot, "result");
    cJSON *xs = cJSON_GetObjectItem(result, "xs");
    cJSON *ys = cJSON_GetObjectItem(result, "ys");
    int nx = cJSON_GetArraySize(xs);
    if (!cJSON_IsArray(xs) || !cJSON_IsArray(ys) || nx < 2 ||
        nx != cJSON_GetArraySize(ys)) {
        cJSON_Delete(rroot);
        return btool_fail(res, AIRY_ERR_EXEC_FAIL,
                          "maths_plot: unexpected response shape");
    }
    size_t cap = strlen(expr_str) + 512 + 32 * (size_t)nx;
    char *out = (char *)AIRY_MALLOC(cap);
    if (!out) {
        cJSON_Delete(rroot);
        return btool_fail(res, AIRY_ERR_GENERIC_FAIL, "OOM");
    }
    int off = snprintf(out, cap, "```plot\ntitle: %s\nxs: ", expr_str);
    off = plot_series(out, cap, off, xs, 0);
    off = maths_plot_append(out, cap, off, "\nys: ");
    off = plot_series(out, cap, off, ys, 1);
    off = maths_plot_append(out, cap, off, "\n```");
    cJSON_Delete(rroot);
    if (off < 0) {
        AIRY_FREE(out);
        return btool_fail(res, AIRY_ERR_EXEC_FAIL, "maths_plot: output overflow");
    }
    return btool_okout(res, out);
}
#endif /* _WIN32 */

/**
 * @brief maths_eval — 数学表达式求值（委托 maths_d）
 * 参数：{"expression":"sqrt(144)+2"}  或  {"expr":"..."}
 */
int maths_eval_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    (void)timeout_ms;
    if (!res) return AIRY_ERR_INVALID_PARAM;
#ifdef _WIN32
    return btool_fail(res, AIRY_ERR_NOT_SUPPORTED,
                      "maths_eval is not supported on Windows");
#else
    /* CJSON_PARSE_GUARD 使用 CJSON_AUTO_FREE（作用域结束时自动释放），
     * 因此本函数内不得再显式 cJSON_Delete(root)。 */
    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });

    cJSON *expr = cJSON_GetObjectItem(root, "expression");
    if (!cJSON_IsString(expr))
        expr = cJSON_GetObjectItem(root, "expr");
    if (!cJSON_IsString(expr) || !expr->valuestring || !expr->valuestring[0])
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "Missing string parameter: expression");
    if (strlen(expr->valuestring) > 4096)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "expression too long (max 4096 chars)");

    /* cJSON 序列化 expression，保证 JSON 转义正确 */
    cJSON *params_obj = cJSON_CreateObject();
    if (!params_obj) return btool_fail(res, AIRY_ERR_GENERIC_FAIL, "OOM");
    cJSON_AddItemToObject(params_obj, "expr",
                          cJSON_CreateString(expr->valuestring));
    char resp[MATHS_RESP_CAP];
    int rc = maths_send("eval", params_obj, resp, sizeof(resp), res);
    if (rc != 0) return rc;
    return maths_scalar(resp, "maths_eval", res);
#endif
}

/**
 * @brief maths_plot — 函数绘图采样（委托 maths_d plot RPC）
 * 参数：{"expression":"sin(x)+x/2","xmin":-10,"xmax":10,"samples":128}
 * 输出 ```plot fenced 块（title/xs/ys），供模型原文嵌入回复、TUI 画板渲染。
 */
int maths_plot_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    (void)timeout_ms;
    if (!res) return AIRY_ERR_INVALID_PARAM;
#ifdef _WIN32
    return btool_fail(res, AIRY_ERR_NOT_SUPPORTED,
                      "maths_plot is not supported on Windows");
#else
    /* CJSON_PARSE_GUARD 使用 CJSON_AUTO_FREE（作用域结束时自动释放），
     * 因此本函数内不得再显式 cJSON_Delete(root)。 */
    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });

    cJSON *expr = NULL;
    cJSON *xmin = NULL;
    cJSON *xmax = NULL;
    int rc = plot_check(root, &expr, &xmin, &xmax, res);
    if (rc != AIRY_OK) return rc;
    cJSON *params_obj = plot_params(expr, xmin, xmax, root);
    if (!params_obj) return btool_fail(res, AIRY_ERR_GENERIC_FAIL, "OOM");
    char resp[MATHS_RESP_CAP];
    rc = maths_send("plot", params_obj, resp, sizeof(resp), res);
    if (rc != 0) return rc;
    cJSON *rroot = maths_parse_response(resp, res);
    if (!rroot)
        return AIRY_ERR_EXEC_FAIL;
    return plot_emit(expr->valuestring, rroot, res);
#endif
}

/**
 * @brief maths_stats — 描述性统计（委托 maths_d）
 * 参数：{"op":"mean|median|variance|stddev|sum|min|max","values":[1,2,3]}
 */
int maths_stats_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    (void)timeout_ms;
    if (!res)
        return AIRY_ERR_INVALID_PARAM;
#ifdef _WIN32
    return btool_fail(res, AIRY_ERR_NOT_SUPPORTED,
                      "maths_stats is not supported on Windows");
#else
    /* CJSON_PARSE_GUARD 使用 CJSON_AUTO_FREE（作用域结束时自动释放），
     * 因此本函数内不得再显式 cJSON_Delete(root)。 */
    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });

    cJSON *op = cJSON_GetObjectItem(root, "op");
    cJSON *values = cJSON_GetObjectItem(root, "values");
    if (!cJSON_IsString(op) || !op->valuestring || !op->valuestring[0] ||
        !cJSON_IsArray(values) || cJSON_GetArraySize(values) == 0)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "Missing params: op (string) + values (non-empty "
                          "array)");

    cJSON *params_obj = cJSON_CreateObject();
    if (!params_obj)
        return btool_fail(res, AIRY_ERR_GENERIC_FAIL, "OOM");
    cJSON_AddItemToObject(params_obj, "op", cJSON_CreateString(op->valuestring));
    cJSON *vals_arr = cJSON_CreateArray();
    if (!vals_arr) {
        cJSON_Delete(params_obj);
        return btool_fail(res, AIRY_ERR_GENERIC_FAIL, "OOM");
    }
    cJSON_AddItemToObject(params_obj, "values", vals_arr);
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, values) {
        if (cJSON_IsNumber(item))
            cJSON_AddItemToArray(vals_arr, cJSON_CreateNumber(item->valuedouble));
    }
    if (cJSON_GetArraySize(vals_arr) == 0) {
        cJSON_Delete(params_obj);
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "values[] must contain numbers");
    }
    char resp[MATHS_RESP_CAP];
    int rc = maths_send("stats", params_obj, resp, sizeof(resp), res);
    if (rc != 0)
        return rc;
    return maths_scalar(resp, "maths_stats", res);
#endif
}
