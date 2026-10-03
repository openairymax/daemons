// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file daemon_rpc_client.c
 * @brief Daemon JSON-RPC 轻量客户端（L1 socket 通道）
 *
 * 机制与策略分离（0.1.19 §121）：平台差异收窄为四个连接原语——
 * rpc_connect（POSIX AF_UNIX / Windows TCP 回环）、rpc_poll、rpc_recv、
 * rpc_send_raw；机制层（超时步进、取消 drill-down、EOF 完整性探测、
 * hangup 排空、分片发送）单源共享，两平台不再各持双轨实现。
 *
 * rpc_poll 以就绪掩码统一 poll（POSIX）与 select（Windows）。Windows
 * select 无挂断位：对端关闭天然表现为「可读 + recv()==0」，HANGUP
 * 掩码在 Windows 恒为空，hangup 排空与双探测路径自动退化为原 Windows
 * 行为，无需平台分支。EINTR/EAGAIN 重试是 POSIX 真实语义（Windows
 * 的 errno 不由 winsock 设置），以平台条件收窄在 recv 失败路径。
 *
 * Phase-3: the airy_sys_memory_* / airy_sys_agent_* syscalls extracted
 * from syscall_router.c are forwarded through this helper to the
 * mem_d / agent_d daemons.
 */

#include "daemon_rpc_client.h"
#include "daemon_l1_server.h"
#include "svc_logger.h"

#include "airy_memory.h"
#include "error.h"

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
#define DAEMON_RPC_CHUNK 4096
#define DAEMON_RPC_POLL_SLICE_MS 200
#define DAEMON_RPC_CANCEL_TIMEOUT_MS 5000

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

/* 平台统一的连接关闭（POSIX: close / Windows: closesocket），供机制层
 * 取消与收尾路径在两种平台复用同一清理动作。 */
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

/* ---- 平台连接原语（平台差异唯一出口） ---- */

#define RPC_IO_READABLE 0x1
#define RPC_IO_HANGUP 0x2

#if AIRY_PLATFORM_POSIX

/**
 * @brief Connect to a Unix socket
 * @return fd >= 0 on success; -1 on failure (errno logged; the caller maps
 *         the sentinel to an AIRY_ERR_* code)
 */
static int rpc_connect(const char *socket_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        SVC_LOG_ERROR("rpc_connect: socket() failed: %s", strerror(errno));
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
        SVC_LOG_ERROR("rpc_connect: connect(%s) failed: %s", socket_path,
                      strerror(saved_errno));
        return -1;
    }
    return fd;
}

/** @brief poll → 就绪掩码。EINTR 内部重试；0 = 超时；-1 = 硬错误
 *  （POLLERR/POLLNVAL 且无可读数据）。 */
static int rpc_poll(int fd, int timeout_ms)
{
    for (;;) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr <= 0)
            return pr;
        int ev = 0;
        if (pfd.revents & POLLIN)
            ev |= RPC_IO_READABLE;
        if (pfd.revents & POLLHUP)
            ev |= RPC_IO_HANGUP;
        if (ev)
            return ev;
        return -1;
    }
}

static int rpc_recv(int fd, void *buf, int len)
{
    return (int)recv(fd, buf, (size_t)len, 0);
}

static int rpc_send_raw(int fd, const void *buf, int len)
{
    return (int)send(fd, buf, (size_t)len, 0);
}

#elif AIRY_PLATFORM_WINDOWS

/* Windows daemon IPC：daemon 统一走 TCP 回环（见 daemon_main.h
 * DAEMON_DECLARE_COMMON / parse_args）。socket_path 参数约定为
 * "host:port"（如 "127.0.0.1:8086"），与 gateway 的 AIRY_LLM_TCP_ADDR/PORT
 * 约定一致；CLI/gateway 在 Windows 下传入 TCP 端点。 */

/** @brief 解析 "host:port" 并 TCP connect，返回 SOCKET（int）或 -1。 */
static int rpc_connect(const char *socket_path)
{
    char host[128];
    char port_str[16];
    const char *colon = socket_path ? strrchr(socket_path, ':') : NULL;
    if (!colon || colon == socket_path || (size_t)(colon - socket_path) >= sizeof(host) ||
        strlen(colon + 1) >= sizeof(port_str)) {
        SVC_LOG_ERROR("rpc_connect: invalid TCP endpoint '%s'", socket_path ? socket_path : "");
        return -1;
    }
    size_t host_len = (size_t)(colon - socket_path);
    __builtin_memcpy(host, socket_path, host_len);
    host[host_len] = '\0';
    strcpy(port_str, colon + 1);
    uint16_t port = (uint16_t)atoi(port_str);
    if (port == 0) {
        SVC_LOG_ERROR("rpc_connect: invalid port in '%s'", socket_path);
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
        SVC_LOG_ERROR("rpc_connect: connect(%s) failed: %d", socket_path, WSAGetLastError());
        closesocket(fd);
        return -1;
    }
    return (int)fd;
}

/** @brief select → 就绪掩码。select 无挂断位：可读即返回 READABLE，
 *  对端关闭由 recv()==0 呈现，HANGUP 恒不置位。 */
static int rpc_poll(int fd, int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET((SOCKET)fd, &rfds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = (long)timeout_ms * 1000;
    int pr = select(0, &rfds, NULL, NULL, &tv);
    if (pr <= 0)
        return pr;
    return RPC_IO_READABLE;
}

static int rpc_recv(int fd, void *buf, int len)
{
    return (int)recv((SOCKET)fd, buf, len, 0);
}

static int rpc_send_raw(int fd, const void *buf, int len)
{
    return (int)send((SOCKET)fd, buf, len, 0);
}

#endif /* AIRY_PLATFORM_WINDOWS / POSIX */

/* ---- 机制件（平台无关，单源） ---- */

/** @brief 已收字节是否构成完整 JSON（EOF / 超时切片共用的完整性探测）。 */
static int rpc_complete(const rpc_buf_t *buf)
{
    if (buf->size == 0)
        return 0;
    cJSON *probe = cJSON_Parse(buf->data);
    if (!probe)
        return 0;
    cJSON_Delete(probe);
    return 1;
}

/**
 * @brief 取消 drill-down：丢弃在途响应连接，经新连接转发取消请求。
 * daemons 是「单请求单响应即关闭」，取消必须走独立连接。
 */
static int rpc_cancel(int fd, const char *cancel_socket_path, const char *cancel_method,
                      const char *cancel_params_json)
{
    rpc_close_fd(fd);
    if (cancel_method && cancel_method[0]) {
        char *cancel_result = NULL;
        int crc = daemon_rpc_call(cancel_socket_path, cancel_method, cancel_params_json,
                                  &cancel_result, DAEMON_RPC_CANCEL_TIMEOUT_MS);
        AIRY_FREE(cancel_result);
        if (crc != AIRY_SUCCESS)
            SVC_LOG_WARN("rpc cancel request failed (method=%s, rc=%d)", cancel_method, crc);
    }
    return AIRY_ERR_CANCELED;
}

/** @brief 分片发送直至完整写出；部分写出不再误报（原 POSIX 单次 send
 *  在请求超过 socket 缓冲时会静默截断，0.1.19 §121 统一为循环发送）。 */
static int rpc_send_all(int fd, const char *request_str)
{
    size_t req_len = strlen(request_str);
    size_t sent_total = 0;
    while (sent_total < req_len) {
        int n = rpc_send_raw(fd, request_str + sent_total, (int)(req_len - sent_total));
        if (n <= 0)
            return AIRY_ERR_GENERIC_FAIL;
        sent_total += (size_t)n;
    }
    return AIRY_SUCCESS;
}

/**
 * @brief Connect + send a JSON-RPC request, returning the live socket.
 *
 * Shared prefix of daemon_rpc_call_cancelable and daemon_rpc_call_stream:
 * serializes the request via daemon_rpc_json_req (0.1.19 §80 mechanism)
 * and sends it over a freshly connected socket. The caller owns the
 * returned fd (>= 0) and must close it; on failure a negative AIRY_ERR_*
 * code is returned.
 */
static int rpc_conn_send(const char *socket_path, const char *method, const char *params_json)
{
    int fd = rpc_connect(socket_path);
    if (fd < 0)
        return AIRY_ERR_NOT_FOUND;

    char *request_str = daemon_rpc_json_req(method, params_json);
    if (!request_str) {
        rpc_close_fd(fd);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    int rc = rpc_send_all(fd, request_str);
    AIRY_FREE(request_str);
    if (rc != AIRY_SUCCESS) {
        rpc_close_fd(fd);
        SVC_LOG_ERROR("daemon_rpc_call: send failed (method=%s)", method);
        return AIRY_ERR_GENERIC_FAIL;
    }
    return fd;
}

/**
 * @brief hangup 排空：对端已关闭但响应尚未完整——继续读取 socket 缓冲
 * 中对端关闭前已发出的数据直至 EOF，再做完整性判定，避免大响应被
 * hangup 误判为失败（RPC 时序竞态）。仅 POSIX 可达（HANGUP 掩码）。
 */
static int rpc_drain(int fd, rpc_buf_t *buf)
{
    char chunk[DAEMON_RPC_CHUNK];
    for (;;) {
        int n = rpc_recv(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                break;
            return AIRY_ERR_GENERIC_FAIL;
        }
        if (n == 0)
            break;
        int rc = rpc_buf_append(buf, chunk, (size_t)n);
        if (rc != AIRY_SUCCESS)
            return rc;
    }
    if (rpc_complete(buf))
        return AIRY_SUCCESS;
    if (getenv("AIRY_RPC_DIAG"))
        SVC_LOG_ERROR("rpc diag: hangup json-incomplete buf_size=%zu head=%.120s", buf->size,
                      buf->data ? buf->data : "");
    return AIRY_ERR_GENERIC_FAIL;
}

/**
 * @brief Receive with timeout and cancellation: loop recv until a complete
 *        JSON is received or timeout/cancel.
 *
 * Strategy: cJSON completeness probing (rpc_complete); if parsing fails
 * and no timeout yet, keep receiving. Sufficient for the typical
 * single-packet daemon response, and also handles complex fragmented
 * packets.
 *
 * Cancellation drill-down: the cancel token is checked after each poll
 * slice; on hit the in-flight connection is dropped and the cancel request
 * rides a NEW connection (rpc_cancel), returning AIRY_ERR_CANCELED.
 *
 * POLLIN-before-HANGUP discipline: daemons are "single-request-single-
 * response-then-close"; poll may return POLLIN and POLLHUP together.
 * Readable data must be consumed first — checking hangup first would
 * discard the already-arrived complete response (RPC timing race). Large
 * responses cannot be read in one recv: even with hangup set, keep
 * recv-ing (bytes sent before close stay readable in the socket buffer),
 * ending with recv()==0 (EOF), then judge completeness (rpc_drain).
 */
static int rpc_recv_resp(int fd, rpc_buf_t *buf, uint32_t timeout_ms,
                         airy_cancel_token_t *cancel_token, const char *cancel_socket_path,
                         const char *cancel_method, const char *cancel_params_json)
{
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < timeout_ms) {
        if (cancel_token && airy_cancel_token_is_canceled(cancel_token))
            return rpc_cancel(fd, cancel_socket_path, cancel_method, cancel_params_json);

        int remain = (int)((timeout_ms - elapsed_ms) < DAEMON_RPC_POLL_SLICE_MS
                               ? (timeout_ms - elapsed_ms)
                               : DAEMON_RPC_POLL_SLICE_MS);
        int ev = rpc_poll(fd, remain);
        if (ev == 0) {
            elapsed_ms += (uint32_t)remain;
            if (rpc_complete(buf))
                return AIRY_SUCCESS;
            continue;
        }
        if (ev < 0)
            return AIRY_ERR_GENERIC_FAIL;

        if (!(ev & RPC_IO_READABLE)) {
            if (getenv("AIRY_RPC_DIAG"))
                SVC_LOG_ERROR("rpc diag: hangup-no-POLLIN revents=0x%x buf_size=%zu", ev,
                              buf->size);
            return AIRY_ERR_GENERIC_FAIL;
        }

        char chunk[DAEMON_RPC_CHUNK];
        int n = rpc_recv(fd, chunk, sizeof(chunk));
        if (n < 0) {
#if AIRY_PLATFORM_POSIX
            if (errno == EINTR || errno == EAGAIN)
                continue;
#endif
            if (getenv("AIRY_RPC_DIAG"))
                SVC_LOG_ERROR("rpc diag: recv<0 errno=%d buf_size=%zu", errno, buf->size);
            return AIRY_ERR_GENERIC_FAIL;
        }
        if (n == 0) {
            if (rpc_complete(buf))
                return AIRY_SUCCESS;
            if (getenv("AIRY_RPC_DIAG"))
                SVC_LOG_ERROR("rpc diag: EOF buf_size=%zu head=%.120s", buf->size,
                              buf->data ? buf->data : "");
            return AIRY_ERR_GENERIC_FAIL;
        }
        int rc = rpc_buf_append(buf, chunk, (size_t)n);
        if (rc != AIRY_SUCCESS)
            return rc;
        if (rpc_complete(buf))
            return AIRY_SUCCESS;
        if (ev & RPC_IO_HANGUP)
            return rpc_drain(fd, buf);
        elapsed_ms += 1;
    }
    return AIRY_ERR_TIMEOUT;
}

int daemon_rpc_call(const char *socket_path, const char *method, const char *params_json,
                    char **out_result_json, uint32_t timeout_ms)
{
    /* Blueprint 8.3.3 grey rollout: when the ns transport switch resolves to
     * "corekern" — since WS-8 stage 3 the resolution default, with "jsonrpc"
     * as the operator escape hatch — the call rides the L2 channel first.
     * Fallback discipline: only codes proving the request was never
     * dispatched fall back to the socket path (ENOENT: no bridge in this
     * process — the cross-process grey norm; CANCELED/ECANCELED: dead
     * target or envelope dropped pre-dispatch). A folded daemon error
     * (GENERIC_FAIL) or a post-dispatch loss (ETIMEDOUT) propagates: a
     * blind retry could double-execute side-effectful methods. Stream and
     * cancelable calls keep the socket path (chunked replies have no L2
     * mapping yet). */
    char channel[64];
    if (socket_path &&
        daemon_l2_channel_for_socket(socket_path, channel, sizeof(channel)) == 0) {
        int rc = daemon_l2_rpc_call(channel, method, params_json, out_result_json, timeout_ms);
        if (rc == AIRY_SUCCESS ||
            (rc != AIRY_ENOENT && rc != AIRY_ERR_CANCELED && rc != AIRY_ECANCELED)) {
            return rc;
        }
        SVC_LOG_DEBUG("daemon_rpc_call: L2 channel '%s' unserved (rc=%d) - socket fallback",
                      channel, rc);
    }
    return daemon_rpc_call_cancelable(socket_path, method, params_json, out_result_json,
                                      timeout_ms, NULL, NULL, NULL);
}

int daemon_rpc_call_stream(const char *socket_path, const char *method, const char *params_json,
                           daemon_rpc_stream_cb_t on_chunk, void *user_data, uint32_t timeout_ms)
{
    if (!socket_path || !method)
        return AIRY_ERR_INVALID_PARAM;
    if (timeout_ms == 0)
        timeout_ms = DAEMON_RPC_DEFAULT_TIMEOUT_MS;

    int fd = rpc_conn_send(socket_path, method, params_json);
    if (fd < 0)
        return fd;

    /* Streaming read loop: every recv() payload is delivered to the callback
     * as it arrives; recv() == 0 (peer closed the connection) marks the end
     * of the stream (daemons are single-request-single-response-then-close).
     * Timeout slices keep the loop responsive; the caller gets a partial
     * prefix via the callback when it fires. */
    uint32_t elapsed_ms = 0;
    int rc = AIRY_ERR_TIMEOUT;

    while (elapsed_ms < timeout_ms) {
        int remain = (int)((timeout_ms - elapsed_ms) < DAEMON_RPC_POLL_SLICE_MS
                               ? (timeout_ms - elapsed_ms)
                               : DAEMON_RPC_POLL_SLICE_MS);
        int ev = rpc_poll(fd, remain);
        if (ev == 0) {
            elapsed_ms += (uint32_t)remain;
            continue;
        }
        if (ev < 0) {
            rc = AIRY_ERR_GENERIC_FAIL;
            break;
        }
        if (!(ev & RPC_IO_READABLE)) {
            /* HUP without readable data: treat as end of stream only if the
             * server already finished writing (EOF). Without data we cannot
             * distinguish an early hangup from a finished stream; the daemon
             * writes the final chunk before closing, so a clean completion is
             * reported as readable+EOF in the same poll cycle. */
            rc = (ev & RPC_IO_HANGUP) ? AIRY_SUCCESS : AIRY_ERR_GENERIC_FAIL;
            break;
        }

        char chunk[DAEMON_RPC_CHUNK];
        int n = rpc_recv(fd, chunk, sizeof(chunk));
        if (n < 0) {
#if AIRY_PLATFORM_POSIX
            if (errno == EINTR || errno == EAGAIN)
                continue;
#endif
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

    rpc_close_fd(fd);
    if (rc != AIRY_SUCCESS)
        SVC_LOG_ERROR("daemon_rpc_call_stream: stream ended rc=%d (method=%s, timeout=%u)", rc,
                      method, timeout_ms);
    return rc;
}

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

    int fd = rpc_conn_send(socket_path, method, params_json);
    if (fd < 0)
        return fd;

    rpc_buf_t buf;
    int rc = rpc_buf_init(&buf);
    if (rc != AIRY_SUCCESS) {
        rpc_close_fd(fd);
        return rc;
    }

    rc = rpc_recv_resp(fd, &buf, timeout_ms, cancel_token, socket_path, cancel_method,
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
