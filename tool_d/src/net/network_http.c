// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 *
 * @file network_http.c
 * @brief Network module - HTTP request domain.
 *
 * Implements the HTTP request construction/send/response parsing and
 * release functions declared in network_common.h, following the design
 * principles of ARCHITECTURAL_PRINCIPLES.md:
 * - E-4 Cross-platform consistency: Windows/Linux/macOS
 * - E-5 Semantic naming: every function name states its purpose
 * - E-6 Traceable errors: unified error code system
 */

#include "network_common_internal.h"

/* snprintf 返回的是「本应写入」长度，超限时该值大于剩余容量。若直接累加
 * offset，后续 cap - offset 会下溢成巨大值，越界写穿栈缓冲。所有片段都经
 * 此一处判定并就地回报，调用点不再各自检查。 */
static airy_err_t head_put(char *buf, size_t cap, int *off, network_http_response_t *resp,
                           const char *fmt, ...)
{
    va_list ap;
    int wlen;

    va_start(ap, fmt);
    wlen = vsnprintf(buf + *off, cap - (size_t)*off, fmt, ap);
    va_end(ap);
    if (wlen < 0 || (size_t)wlen >= cap - (size_t)*off) {
        resp->error = AIRY_ERR_OVERFLOW;
        resp->error_message = AIRY_STRDUP("HTTP request header too long");
        return AIRY_ERR_OVERFLOW;
    }
    *off += wlen;
    return AIRY_SUCCESS;
}

/* 可选头部：值为空视为不发送，使调用点只剩「出错才返回」一条判断。 */
static airy_err_t head_opt(char *buf, size_t cap, int *off, network_http_response_t *resp,
                           const char *fmt, const char *val)
{
    if (!val)
        return AIRY_SUCCESS;
    return head_put(buf, cap, off, resp, fmt, val);
}

static airy_err_t head_list(char *buf, size_t cap, int *off, network_http_response_t *resp,
                            const network_http_request_t *req)
{
    airy_err_t rc = AIRY_SUCCESS;

    if (!req->headers || req->header_count == 0)
        return AIRY_SUCCESS;
    for (size_t i = 0; i < req->header_count && rc == AIRY_SUCCESS; i++)
        rc = head_opt(buf, cap, off, resp, "%s\r\n", req->headers[i]);
    return rc;
}

static airy_err_t http_head(network_connection_t *conn, const network_http_request_t *req,
                            char *buf, size_t cap, size_t *out_len, network_http_response_t *resp)
{
    int off = 0;
    airy_err_t rc;

    rc = head_put(buf, cap, &off, resp, "%s %s HTTP/1.1\r\n",
                  req->method ? req->method : "GET", req->path ? req->path : "/");
    if (rc == AIRY_SUCCESS)
        rc = head_opt(buf, cap, &off, resp, "Host: %s\r\n", conn->config.host);
    if (rc == AIRY_SUCCESS)
        rc = head_opt(buf, cap, &off, resp, "Content-Type: %s\r\n", req->content_type);
    if (rc == AIRY_SUCCESS && req->body && req->body_len > 0)
        rc = head_put(buf, cap, &off, resp, "Content-Length: %zu\r\n", req->body_len);
    if (rc == AIRY_SUCCESS)
        rc = head_list(buf, cap, &off, resp, req);
    if (rc == AIRY_SUCCESS)
        rc = head_put(buf, cap, &off, resp, "\r\n");
    *out_len = (size_t)off;
    return rc;
}

static airy_err_t http_send(network_connection_t *conn, const network_http_request_t *req,
                            network_http_response_t *resp)
{
    char buf[NETWORK_DEFAULT_BUFFER_SIZE * 2];
    size_t len = 0;
    airy_err_t err;

    err = http_head(conn, req, buf, sizeof(buf), &len, resp);
    if (err != AIRY_SUCCESS)
        return err;
    err = network_send_all(conn, buf, len);
    if (err != AIRY_SUCCESS) {
        resp->error = err;
        resp->error_message = AIRY_STRDUP("Failed to send request headers");
        return err;
    }
    if (!req->body || req->body_len == 0)
        return AIRY_SUCCESS;
    err = network_send_all(conn, req->body, req->body_len);
    if (err != AIRY_SUCCESS) {
        resp->error = err;
        resp->error_message = AIRY_STRDUP("Failed to send request body");
    }
    return err;
}

/* 收满一轮响应。连接被重置或连续 10 次读失败时停止重试，已收字节保留，
 * 由解析阶段按实际长度处理——短响应不等于传输失败。 */
static size_t http_recv(network_connection_t *conn, char *buf, size_t cap)
{
    size_t total = 0;
    size_t got;
    int retry = 0;
    airy_err_t err;

    do {
        got = 0;
        err = network_receive(conn, buf + total, cap - total - 1, &got);
        if (err == AIRY_SUCCESS && got > 0) {
            total += got;
            retry = 0;
        } else if (err != AIRY_SUCCESS) {
            if (++retry > 10 || err == AIRY_ECONNRESET)
                break;
        }
    } while (got > 0 && total < cap - 1);
    buf[total] = '\0';
    return total;
}

/* 状态行缺失或格式不符时按 200 处理：本模块只做轻量客户端，
 * 非标准响应体沿用该约定，避免把可达的传输误判为失败。 */
static int http_status(const char *buf, size_t len)
{
    if (len < 12 || __builtin_strncmp(buf, "HTTP/1.", 7) != 0 || buf[8] != ' ')
        return 200;
    return (int)strtol(buf + 9, NULL, 10);
}

static void http_split(network_http_response_t *resp, const char *buf, size_t total)
{
    const char *body = strstr(buf, "\r\n\r\n");
    size_t hlen;

    if (!body) {
        resp->body = AIRY_MALLOC(total + 1);
        if (resp->body) {
            __builtin_memcpy(resp->body, buf, total);
            ((char *)resp->body)[total] = '\0';
            resp->body_len = total;
        }
        return;
    }
    hlen = (size_t)(body - buf) + 4;
    resp->headers = (char **)AIRY_CALLOC(1, sizeof(char *));
    if (resp->headers) {
        resp->headers[0] = (char *)AIRY_MALLOC(hlen + 1);
        if (resp->headers[0]) {
            __builtin_memcpy(resp->headers[0], buf, hlen);
            resp->headers[0][hlen] = '\0';
        }
        resp->header_count = 1;
    }
    body += 4;
    total -= (size_t)(body - buf);
    resp->body = AIRY_MALLOC(total + 1);
    if (resp->body) {
        __builtin_memcpy(resp->body, body, total);
        ((char *)resp->body)[total] = '\0';
        resp->body_len = total;
    }
}

airy_err_t network_http_request(network_connection_t *connection,
                                const network_http_request_t *request,
                                network_http_response_t *response)
{
    char recv_buffer[65536];
    size_t total;
    airy_err_t err;

    if (!connection || !request || !response) {
        return AIRY_EINVAL;
    }

    if (connection->status != NETWORK_STATUS_CONNECTED) {
        return AIRY_ENOTCONN;
    }

    AIRY_MEMSET(response, 0, sizeof(network_http_response_t));

    err = http_send(connection, request, response);
    if (err != AIRY_SUCCESS)
        return err;

    total = http_recv(connection, recv_buffer, sizeof(recv_buffer));
    response->status_code = http_status(recv_buffer, total);
    http_split(response, recv_buffer, total);
    response->error = AIRY_SUCCESS;
    return AIRY_SUCCESS;
}

airy_err_t network_http_get(network_connection_t *connection, const char *path,
                            network_http_response_t *response)
{
    network_http_request_t request = {0};
    request.method = "GET";
    request.path = path;

    return network_http_request(connection, &request, response);
}

airy_err_t network_http_post(network_connection_t *connection, const char *path,
                             const char *content_type, const void *body, size_t body_len,
                             network_http_response_t *response)
{
    network_http_request_t request = {0};
    request.method = "POST";
    request.path = path;
    request.content_type = content_type ? content_type : "application/json";
    request.body = body;
    request.body_len = body_len;

    return network_http_request(connection, &request, response);
}

void network_http_response_free(network_http_response_t *response)
{
    if (!response) {
        return;
    }

    if (response->body) {
        AIRY_FREE(response->body);
        response->body = NULL;
    }

    if (response->headers) {
        for (size_t i = 0; i < response->header_count; i++) {
            if (response->headers[i]) {
                AIRY_FREE(response->headers[i]);
            }
        }
        AIRY_FREE(response->headers);
        response->headers = NULL;
    }

    if (response->error_message) {
        AIRY_FREE(response->error_message);
        response->error_message = NULL;
    }

    if (response->status_text) {
        AIRY_FREE(response->status_text);
        response->status_text = NULL;
    }

    AIRY_MEMSET(response, 0, sizeof(network_http_response_t));
}
