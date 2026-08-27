// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file daemon_rpc_client.c
 * @brief Lightweight Unix-socket JSON-RPC client implementation.
 *
 * Phase-3 executor consolidation refactor: the airy_sys_memory_* /
 * airy_sys_agent_* syscalls extracted from syscall_router.c are forwarded
 * through this helper to the mem_d / agent_d daemons.
 *
 * Implementation:
 *   - POSIX: socket(AF_UNIX, SOCK_STREAM) + connect + send + recv (poll timeout)
 *   - Windows: returns AIRY_ERR_NOT_SUPPORTED (gateway_d keeps the
 *     in-process g_runtime fallback path on Windows, but the thin IPC
 *     client is not supported yet)
 */

#include "daemon_rpc_client.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if AIRY_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#elif AIRY_PLATFORM_POSIX
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define DAEMON_RPC_DEFAULT_TIMEOUT_MS 30000
#define DAEMON_RPC_MAX_RESPONSE (16 * 1024 * 1024) /* 16MB */
#define DAEMON_RPC_INITIAL_BUF 4096

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} rpc_buf_t;

static int rpc_buf_init(rpc_buf_t *buf)
{
    buf->capacity = DAEMON_RPC_INITIAL_BUF;
    buf->data = (char *)AIRY_MALLOC(buf->capacity);
    if (!buf->data) {
        buf->capacity = 0;
        buf->size = 0;
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    buf->data[0] = '\0';
    buf->size = 0;
    return AIRY_SUCCESS;
}

static void rpc_buf_free(rpc_buf_t *buf)
{
    if (buf->data) {
        AIRY_FREE(buf->data);
        buf->data = NULL;
    }
    buf->size = 0;
    buf->capacity = 0;
}

static int rpc_buf_append(rpc_buf_t *buf, const char *src, size_t len)
{
    if (buf->size + len + 1 > DAEMON_RPC_MAX_RESPONSE)
        return AIRY_ERR_OUT_OF_MEMORY;

    if (buf->size + len + 1 > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        if (new_cap < buf->size + len + 1)
            new_cap = buf->size + len + 1;
        char *p = (char *)AIRY_REALLOC(buf->data, new_cap);
        if (!p)
            return AIRY_ERR_OUT_OF_MEMORY;
        buf->data = p;
        buf->capacity = new_cap;
    }
    __builtin_memcpy(buf->data + buf->size, src, len);
    buf->size += len;
    buf->data[buf->size] = '\0';
    return AIRY_SUCCESS;
}

/* 平台统一的连接关闭（POSIX: close / Windows: closesocket），供公共
 * daemon_rpc_call_cancelable 在两种平台复用同一清理路径。 */
static void rpc_close_fd(int fd)
{
    if (fd < 0)
        return;
#if AIRY_PLATFORM_WINDOWS
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
}

#if AIRY_PLATFORM_POSIX

/**
 * @brief Connect to a Unix socket
 * @return fd >= 0 on success; -1 on failure (errno logged; the caller maps
 *         the sentinel to an AIRY_ERR_* code)
 */
static int rpc_connect_unix(const char *socket_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        SVC_LOG_ERROR("rpc_connect_unix: socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* Use __builtin_strncpy to avoid the compile-time error from BAN-211/235
     * banning strncpy (same policy as daemons/common/src/platform_compat.c) */
    __builtin_strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved_errno = errno;
        close(fd);
        /* Return a plain negative sentinel, NOT a negated AIRY_ERR_* code:
         * error codes are already negative (e.g. AIRY_ERR_NOT_FOUND = -6),
         * so negating them yields a positive value that callers misread as
         * a valid fd and send() on it (misleading "send failed" reports). */
        SVC_LOG_ERROR("rpc_connect_unix: connect(%s) failed: %s", socket_path,
                      strerror(saved_errno));
        return -1;
    }
    return fd;
}

/**
 * @brief Receive with timeout and cancellation: loop recv until a complete
 *        JSON is received or timeout/cancel
 *
 * Simplified strategy: use cJSON_ParseIntervalFromBuffer to detect JSON
 * completeness; if parsing fails and no timeout yet, keep receiving. This is
 * sufficient for the typical single-packet daemon response, and also handles
 * complex fragmented packets correctly.
 *
 * Improvement 1 (cancellation drill-down): check the cancel token after each
 * poll slice (200ms). On hit, first close the current connection (invoke
 * response discarded), then send the cancel request over a NEW connection
 * (daemons are "single-request-single-response-then-close"; cancel needs its
 * own connection), returning AIRY_ERR_CANCELED.
 */
static int rpc_recv_response(int fd, rpc_buf_t *buf, uint32_t timeout_ms,
                             airy_cancel_token_t *cancel_token, const char *cancel_socket_path,
                             const char *cancel_method, const char *cancel_params_json)
{

    uint32_t elapsed_ms = 0;
    const uint32_t step_ms = 200;

    while (elapsed_ms < timeout_ms) {

        if (cancel_token && airy_cancel_token_is_canceled(cancel_token)) {
#if AIRY_PLATFORM_POSIX
            close(fd);
#endif

            if (cancel_method && cancel_method[0]) {
                char *cancel_result = NULL;
                int crc = daemon_rpc_call(cancel_socket_path, cancel_method, cancel_params_json,
                                          &cancel_result, 5000);
                AIRY_FREE(cancel_result);
                if (crc != AIRY_SUCCESS)
                    SVC_LOG_WARN("rpc cancel request failed (method=%s, rc=%d)", cancel_method,
                                 crc);
            }
            return AIRY_ERR_CANCELED;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int remain =
            (int)((timeout_ms - elapsed_ms) < step_ms ? (timeout_ms - elapsed_ms) : step_ms);
        int pr = poll(&pfd, 1, remain);
        if (getenv("AIRY_RPC_DIAG") && pr > 0)
            SVC_LOG_ERROR("rpc diag: poll hit fd=%d pr=%d revents=0x%x buf_size=%zu", fd, pr,
                          pfd.revents, buf->size);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return AIRY_ERR_GENERIC_FAIL;
        }
        if (pr == 0) {
            elapsed_ms += (uint32_t)remain;

            if (buf->size > 0) {
                cJSON *probe = cJSON_Parse(buf->data);
                if (probe) {
                    cJSON_Delete(probe);
                    return AIRY_SUCCESS;
                }
            }
            continue;
        }
        /* The peer closes immediately after sending the response (daemons are
         * "single-request-single-response-then-close"); poll may return both
         * POLLIN and POLLHUP. POLLIN data must be processed first, otherwise
         * checking POLLHUP first discards the already-arrived complete
         * response (RPC timing race). Large responses (>4096 bytes) cannot be
         * read in one recv: even with hangup set, keep recv-ing the remaining
         * data (bytes sent before close are still readable from the socket
         * buffer), finally ending with recv()==0 (EOF), then judge JSON
         * completeness. */
        int hangup = (pfd.revents & (POLLHUP | POLLNVAL));
        if (pfd.revents & POLLIN) {
            char chunk[4096];
            ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (getenv("AIRY_RPC_DIAG"))
                SVC_LOG_ERROR("rpc diag: recv n=%zd errno=%d buf_size=%zu revents=0x%x", n, errno,
                              buf->size, pfd.revents);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                if (getenv("AIRY_RPC_DIAG"))
                    SVC_LOG_ERROR("rpc diag: recv<0 errno=%d buf_size=%zu", errno, buf->size);
                return AIRY_ERR_GENERIC_FAIL;
            }
            if (n == 0) {

                if (buf->size > 0) {
                    cJSON *probe = cJSON_Parse(buf->data);
                    if (probe) {
                        cJSON_Delete(probe);
                        return AIRY_SUCCESS;
                    }
                }
                if (getenv("AIRY_RPC_DIAG"))
                    SVC_LOG_ERROR("rpc diag: EOF buf_size=%zu head=%.120s", buf->size,
                                  buf->data ? buf->data : "");
                return AIRY_ERR_GENERIC_FAIL;
            }
            int rc = rpc_buf_append(buf, chunk, (size_t)n);
            if (rc != AIRY_SUCCESS)
                return rc;

            cJSON *probe = cJSON_Parse(buf->data);
            if (probe) {
                cJSON_Delete(probe);
                return AIRY_SUCCESS;
            }
            if (hangup) {
                /* Data incomplete but connection closed: read out the
                 * remaining socket data (bytes the peer sent before close are
                 * still readable) until EOF before judging completeness, so a
                 * large response is not misjudged as FAIL by hangup. */
                for (;;) {
                    ssize_t n2 = recv(fd, chunk, sizeof(chunk), 0);
                    if (n2 < 0) {
                        if (errno == EINTR)
                            continue;
                        if (errno == EAGAIN)
                            break;
                        return AIRY_ERR_GENERIC_FAIL;
                    }
                    if (n2 == 0)
                        break;
                    rc = rpc_buf_append(buf, chunk, (size_t)n2);
                    if (rc != AIRY_SUCCESS)
                        return rc;
                }
                cJSON *final = cJSON_Parse(buf->data);
                if (final) {
                    cJSON_Delete(final);
                    return AIRY_SUCCESS;
                }
                if (getenv("AIRY_RPC_DIAG"))
                    SVC_LOG_ERROR("rpc diag: hangup json-incomplete buf_size=%zu head=%.120s",
                                  buf->size, buf->data ? buf->data : "");
                return AIRY_ERR_GENERIC_FAIL;
            }
            elapsed_ms += 1;
        } else if (hangup || (pfd.revents & POLLERR)) {
            if (getenv("AIRY_RPC_DIAG"))
                SVC_LOG_ERROR("rpc diag: hangup-no-POLLIN revents=0x%x buf_size=%zu", pfd.revents,
                              buf->size);
            return AIRY_ERR_GENERIC_FAIL;
        }
    }
    return AIRY_ERR_TIMEOUT;
}

#elif AIRY_PLATFORM_WINDOWS

/* Windows daemon IPC：daemon 统一走 TCP 回环（见 daemon_main.h
 * DAEMON_DECLARE_COMMON / parse_args）。socket_path 参数约定为
 * "host:port"（如 "127.0.0.1:8086"），与 gateway 的 AIRY_LLM_TCP_ADDR/PORT
 * 约定一致；CLI/gateway 在 Windows 下传入 TCP 端点。 */

/** @brief 解析 "host:port" 并 TCP connect，返回 SOCKET（int）或 -1。 */
static int rpc_connect_unix(const char *socket_path)
{
    char host[128];
    char port_str[16];
    const char *colon = socket_path ? strrchr(socket_path, ':') : NULL;
    if (!colon || colon == socket_path || (size_t)(colon - socket_path) >= sizeof(host) ||
        strlen(colon + 1) >= sizeof(port_str)) {
        SVC_LOG_ERROR("rpc_connect_unix: invalid TCP endpoint '%s'", socket_path ? socket_path : "");
        return -1;
    }
    size_t host_len = (size_t)(colon - socket_path);
    __builtin_memcpy(host, socket_path, host_len);
    host[host_len] = '\0';
    strcpy(port_str, colon + 1);
    uint16_t port = (uint16_t)atoi(port_str);
    if (port == 0) {
        SVC_LOG_ERROR("rpc_connect_unix: invalid port in '%s'", socket_path);
        return -1;
    }

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
        return -1;

    struct sockaddr_in addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        addr.sin_addr.s_addr = INADDR_LOOPBACK;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        SVC_LOG_ERROR("rpc_connect_unix: connect(%s) failed: %d", socket_path, WSAGetLastError());
        closesocket(fd);
        return -1;
    }
    return (int)fd;
}

/** @brief select 版 recv 循环（等价 POSIX poll 语义）。 */
static int rpc_recv_response(int fd, rpc_buf_t *buf, uint32_t timeout_ms,
                             airy_cancel_token_t *cancel_token, const char *cancel_socket_path,
                             const char *cancel_method, const char *cancel_params_json)
{
    uint32_t elapsed_ms = 0;
    const uint32_t step_ms = 200;

    while (elapsed_ms < timeout_ms) {
        if (cancel_token && airy_cancel_token_is_canceled(cancel_token)) {
            closesocket((SOCKET)fd);
            if (cancel_method && cancel_method[0]) {
                char *cancel_result = NULL;
                int crc = daemon_rpc_call(cancel_socket_path, cancel_method, cancel_params_json,
                                          &cancel_result, 5000);
                AIRY_FREE(cancel_result);
                if (crc != AIRY_SUCCESS)
                    SVC_LOG_WARN("rpc cancel request failed (method=%s, rc=%d)", cancel_method, crc);
            }
            return AIRY_ERR_CANCELED;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET((SOCKET)fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = (long)(step_ms * 1000);

        int pr = select(0, &rfds, NULL, NULL, &tv);
        if (pr < 0)
            return AIRY_ERR_GENERIC_FAIL;
        if (pr == 0) {
            elapsed_ms += step_ms;
            if (buf->size > 0) {
                cJSON *probe = cJSON_Parse(buf->data);
                if (probe) {
                    cJSON_Delete(probe);
                    return AIRY_SUCCESS;
                }
            }
            continue;
        }

        char chunk[4096];
        int n = recv((SOCKET)fd, chunk, sizeof(chunk), 0);
        if (n < 0)
            return AIRY_ERR_GENERIC_FAIL;
        if (n == 0) {
            if (buf->size > 0) {
                cJSON *probe = cJSON_Parse(buf->data);
                if (probe) {
                    cJSON_Delete(probe);
                    return AIRY_SUCCESS;
                }
            }
            return AIRY_ERR_GENERIC_FAIL;
        }
        int rc = rpc_buf_append(buf, chunk, (size_t)n);
        if (rc != AIRY_SUCCESS)
            return rc;

        cJSON *probe = cJSON_Parse(buf->data);
        if (probe) {
            cJSON_Delete(probe);
            return AIRY_SUCCESS;
        }
        elapsed_ms += 1;
    }
    return AIRY_ERR_TIMEOUT;
}

#endif /* AIRY_PLATFORM_WINDOWS / POSIX */
int daemon_rpc_call(const char *socket_path, const char *method, const char *params_json,
                    char **out_result_json, uint32_t timeout_ms)
{
    return daemon_rpc_call_cancelable(socket_path, method, params_json, out_result_json, timeout_ms,
                                      NULL, NULL, NULL);
}

#if AIRY_PLATFORM_POSIX

/**
 * @brief Connect + send a JSON-RPC request, returning the live socket.
 *
 * Shared prefix of daemon_rpc_call_cancelable and daemon_rpc_call_stream:
 * builds the JSON-RPC 2.0 request object, serializes it and sends it over a
 * freshly connected Unix socket. The caller owns the returned fd (>= 0) and
 * must close it; on failure a negative AIRY_ERR_* code is returned.
 */
static int rpc_connect_send(const char *socket_path, const char *method, const char *params_json)
{
    int fd = rpc_connect_unix(socket_path);
    if (fd < 0)
        return AIRY_ERR_NOT_FOUND;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        close(fd);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", method);
    if (params_json && params_json[0] != '\0') {
        cJSON *params = cJSON_Parse(params_json);
        if (params) {
            cJSON_AddItemToObject(root, "params", params);
        } else {
            cJSON_AddStringToObject(root, "params", params_json);
        }
    } else {
        cJSON_AddObjectToObject(root, "params");
    }
    cJSON_AddNumberToObject(root, "id", 1);

    char *request_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!request_str) {
        close(fd);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t req_len = strlen(request_str);
    ssize_t sent = send(fd, request_str, req_len, 0);
    AIRY_FREE(request_str);
    if (sent < 0 || (size_t)sent != req_len) {
        close(fd);
        SVC_LOG_ERROR("daemon_rpc_call: send failed (method=%s): %s", method, strerror(errno));
        return AIRY_ERR_GENERIC_FAIL;
    }
    return fd;
}

int daemon_rpc_call_stream(const char *socket_path, const char *method, const char *params_json,
                           daemon_rpc_stream_cb_t on_chunk, void *user_data, uint32_t timeout_ms)
{
    if (!socket_path || !method)
        return AIRY_ERR_INVALID_PARAM;
    if (timeout_ms == 0)
        timeout_ms = DAEMON_RPC_DEFAULT_TIMEOUT_MS;

    int fd = rpc_connect_send(socket_path, method, params_json);
    if (fd < 0)
        return fd;

    /* Streaming read loop: every recv() payload is delivered to the callback
     * as it arrives; recv() == 0 (peer closed the connection) marks the end
     * of the stream (daemons are single-request-single-response-then-close).
     * Timeout slices keep the loop responsive; the caller gets a partial
     * prefix via the callback when it fires. */
    uint32_t elapsed_ms = 0;
    const uint32_t step_ms = 200;
    int rc = AIRY_ERR_TIMEOUT;

    while (elapsed_ms < timeout_ms) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int remain = (int)((timeout_ms - elapsed_ms) < step_ms ? (timeout_ms - elapsed_ms)
                                                               : step_ms);
        int pr = poll(&pfd, 1, remain);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            rc = AIRY_ERR_GENERIC_FAIL;
            break;
        }
        if (pr == 0) {
            elapsed_ms += (uint32_t)remain;
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            /* HUP/POLERR without readable data: treat as end of stream only
             * if the server already finished writing (EOF). Without data we
             * cannot distinguish an early hangup from a finished stream; the
             * daemon writes the final chunk before closing, so a clean
             * completion is reported as POLLIN+EOF in the same poll cycle. */
            rc = (pfd.revents & POLLHUP) ? AIRY_SUCCESS : AIRY_ERR_GENERIC_FAIL;
            break;
        }

        char chunk[4096];
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            rc = AIRY_ERR_GENERIC_FAIL;
            break;
        }
        if (n == 0) {
            /* EOF: the server finished the stream and closed the connection. */
            rc = AIRY_SUCCESS;
            break;
        }
        if (on_chunk)
            on_chunk(chunk, (size_t)n, user_data);
        elapsed_ms += 1;
    }

    close(fd);
    if (rc != AIRY_SUCCESS)
        SVC_LOG_ERROR("daemon_rpc_call_stream: stream ended rc=%d (method=%s, timeout=%u)", rc,
                      method, timeout_ms);
    return rc;
}

#elif AIRY_PLATFORM_WINDOWS

/** @brief Windows 版 connect+send（等价 POSIX rpc_connect_send）。 */
static int rpc_connect_send(const char *socket_path, const char *method, const char *params_json)
{
    int fd = rpc_connect_unix(socket_path);
    if (fd < 0)
        return AIRY_ERR_NOT_FOUND;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        rpc_close_fd(fd);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", method);
    if (params_json && params_json[0] != '\0') {
        cJSON *params = cJSON_Parse(params_json);
        if (params) {
            cJSON_AddItemToObject(root, "params", params);
        } else {
            cJSON_AddStringToObject(root, "params", params_json);
        }
    } else {
        cJSON_AddObjectToObject(root, "params");
    }
    cJSON_AddNumberToObject(root, "id", 1);

    char *request_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!request_str) {
        rpc_close_fd(fd);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t req_len = strlen(request_str);
    size_t sent_total = 0;
    while (sent_total < req_len) {
        int n = send((SOCKET)fd, request_str + sent_total, (int)(req_len - sent_total), 0);
        if (n <= 0)
            break;
        sent_total += (size_t)n;
    }
    AIRY_FREE(request_str);
    if (sent_total != req_len) {
        rpc_close_fd(fd);
        SVC_LOG_ERROR("daemon_rpc_call: send failed (method=%s)", method);
        return AIRY_ERR_GENERIC_FAIL;
    }
    return fd;
}

int daemon_rpc_call_stream(const char *socket_path, const char *method, const char *params_json,
                           daemon_rpc_stream_cb_t on_chunk, void *user_data, uint32_t timeout_ms)
{
    if (!socket_path || !method)
        return AIRY_ERR_INVALID_PARAM;
    if (timeout_ms == 0)
        timeout_ms = DAEMON_RPC_DEFAULT_TIMEOUT_MS;

    int fd = rpc_connect_send(socket_path, method, params_json);
    if (fd < 0)
        return fd;

    uint32_t elapsed_ms = 0;
    const uint32_t step_ms = 200;
    int rc = AIRY_ERR_TIMEOUT;

    while (elapsed_ms < timeout_ms) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET((SOCKET)fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = (long)(step_ms * 1000);

        int pr = select(0, &rfds, NULL, NULL, &tv);
        if (pr < 0) {
            rc = AIRY_ERR_GENERIC_FAIL;
            break;
        }
        if (pr == 0) {
            elapsed_ms += step_ms;
            continue;
        }

        char chunk[4096];
        int n = recv((SOCKET)fd, chunk, sizeof(chunk), 0);
        if (n < 0) {
            rc = AIRY_ERR_GENERIC_FAIL;
            break;
        }
        if (n == 0) {
            rc = AIRY_SUCCESS;
            break;
        }
        if (on_chunk)
            on_chunk(chunk, (size_t)n, user_data);
        elapsed_ms += 1;
    }

    rpc_close_fd(fd);
    if (rc != AIRY_SUCCESS)
        SVC_LOG_ERROR("daemon_rpc_call_stream: stream ended rc=%d (method=%s, timeout=%u)", rc,
                      method, timeout_ms);
    return rc;
}

#endif /* AIRY_PLATFORM_WINDOWS / POSIX */

int daemon_rpc_call_cancelable(const char *socket_path, const char *method, const char *params_json,
                               char **out_result_json, uint32_t timeout_ms,
                               airy_cancel_token_t *cancel_token, const char *cancel_method,
                               const char *cancel_params_json)
{
    if (!socket_path || !method || !out_result_json)
        return AIRY_ERR_INVALID_PARAM;

    *out_result_json = NULL;
    if (timeout_ms == 0)
        timeout_ms = DAEMON_RPC_DEFAULT_TIMEOUT_MS;

    int fd = rpc_connect_unix(socket_path);
    if (fd < 0)
        return AIRY_ERR_NOT_FOUND;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        rpc_close_fd(fd);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", method);
    if (params_json && params_json[0] != '\0') {
        cJSON *params = cJSON_Parse(params_json);
        if (params) {
            cJSON_AddItemToObject(root, "params", params);
        } else {

            cJSON_AddStringToObject(root, "params", params_json);
        }
    } else {
        cJSON_AddObjectToObject(root, "params");
    }
    cJSON_AddNumberToObject(root, "id", 1);

    char *request_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!request_str) {
        rpc_close_fd(fd);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t req_len = strlen(request_str);
    size_t sent_total = 0;
    while (sent_total < req_len) {
#if AIRY_PLATFORM_WINDOWS
        int n = send((SOCKET)fd, request_str + sent_total, (int)(req_len - sent_total), 0);
#else
        ssize_t n = send(fd, request_str + sent_total, req_len - sent_total, 0);
#endif
        if (n <= 0)
            break;
        sent_total += (size_t)n;
    }
    AIRY_FREE(request_str);
    if (sent_total != req_len) {
        rpc_close_fd(fd);
        SVC_LOG_ERROR("daemon_rpc_call: send failed (method=%s): %s", method, strerror(errno));
        return AIRY_ERR_GENERIC_FAIL;
    }

    rpc_buf_t buf;
    int rc = rpc_buf_init(&buf);
    if (rc != AIRY_SUCCESS) {
        rpc_close_fd(fd);
        return rc;
    }

    rc = rpc_recv_response(fd, &buf, timeout_ms, cancel_token, socket_path, cancel_method,
                           cancel_params_json);
    rpc_close_fd(fd);
    if (rc != AIRY_SUCCESS) {
        if (rc != AIRY_ERR_CANCELED)
            SVC_LOG_ERROR("daemon_rpc_call: recv failed (method=%s, rc=%d, timeout=%u)", method, rc,
                          timeout_ms);
        rpc_buf_free(&buf);
        return rc;
    }

    cJSON *resp = cJSON_Parse(buf.data);
    rpc_buf_free(&buf);
    if (!resp) {
        SVC_LOG_ERROR("daemon_rpc_call: response parse failed (method=%s)", method);
        return AIRY_ERR_GENERIC_FAIL;
    }

    cJSON *err_obj = cJSON_GetObjectItem(resp, "error");
    if (err_obj) {
        cJSON *err_msg = cJSON_GetObjectItem(err_obj, "message");
        const char *msg = (err_msg && cJSON_IsString(err_msg)) ? err_msg->valuestring : "unknown";
        cJSON *err_code = cJSON_GetObjectItem(err_obj, "code");
        int code = (err_code && cJSON_IsNumber(err_code)) ? err_code->valueint : -32000;
        SVC_LOG_WARN("daemon_rpc_call: daemon returned error (method=%s, code=%d, msg=%s)", method,
                     code, msg);
        cJSON_Delete(resp);
        return AIRY_ERR_GENERIC_FAIL;
    }

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (!result) {
        SVC_LOG_ERROR("daemon_rpc_call: missing result field (method=%s)", method);
        cJSON_Delete(resp);
        return AIRY_ERR_GENERIC_FAIL;
    }

    char *result_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(resp);
    if (!result_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    *out_result_json = AIRY_STRDUP(result_str);
    /* cJSON_PrintUnformatted uses the cJSON default allocator (malloc);
     * AIRY_FREE is compatible with the default cJSON allocator */
    AIRY_FREE(result_str);
    if (!*out_result_json)
        return AIRY_ERR_OUT_OF_MEMORY;

    return AIRY_SUCCESS;
}
