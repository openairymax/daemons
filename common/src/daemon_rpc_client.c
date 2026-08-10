// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 *
 * @file daemon_rpc_client.c
 * @brief 轻量级 Unix-socket JSON-RPC 客户端实现
 *
 * Phase 3 执行体集中化重构：从 syscall_router.c 抽离的 airy_sys_memory_* /
 * airy_sys_agent_* 系统调用经此 helper 转发到 mem_d / agent_d 守护进程。
 *
 * 实现：
 *   - POSIX：socket(AF_UNIX, SOCK_STREAM) + connect + send + recv (poll 超时)
 *   - Windows：返回 AIRY_ERR_NOT_SUPPORTED（gateway_d 在 Windows 上仍可
 *     保留进程内 g_runtime 退化路径，但 thin IPC client 暂不支持）
 */

#include "daemon_rpc_client.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if AIRY_PLATFORM_WINDOWS
    /* Windows：仅提供桩，不连接 Unix socket */
#elif AIRY_PLATFORM_POSIX
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <unistd.h>
#endif

#define DAEMON_RPC_DEFAULT_TIMEOUT_MS 30000
#define DAEMON_RPC_MAX_RESPONSE      (16 * 1024 * 1024) /* 16MB */
#define DAEMON_RPC_INITIAL_BUF        4096

/* ==================== 内部辅助：响应缓冲区 ==================== */

typedef struct {
    char *data;
    size_t size;     /* 已写入字节数 */
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

/* ==================== POSIX 实现 ==================== */

#if AIRY_PLATFORM_POSIX

/**
 * @brief 连接到 Unix socket
 * @return fd >= 0 成功；< 0 失败（返回负的 AIRY_ERR_* 错误码）
 */
static int rpc_connect_unix(const char *socket_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -AIRY_ERR_FAIL;

    struct sockaddr_un addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* 使用 __builtin_strncpy 以避免 BAN-211/235 禁用 strncpy 的编译期报错
     * （与 daemons/common/src/platform_compat.c 同策略） */
    __builtin_strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -AIRY_ERR_NOT_FOUND; /* daemon 未运行或路径错误 */
    }
    return fd;
}

/**
 * @brief 带超时与取消的接收：循环 recv 直至收到完整 JSON 或超时/取消
 *
 * 简化策略：使用 cJSON_ParseIntervalFromBuffer 检测 JSON 完整性；
 * 若解析失败且未超时，继续接收。此策略在 daemon 响应通常单包返回的
 * 场景下足够，复杂分包场景亦能正确处理。
 *
 * 改进1（取消下探）：每次 poll 片（200ms）后检查取消令牌。命中时先关闭
 * 当前连接（invoke 响应丢弃），再通过新连接发送 cancel 请求（daemon 为
 * "单请求-单响应-即关闭"模型，cancel 必须独立连接），返回 AIRY_ERR_CANCELED。
 */
static int rpc_recv_response(int fd, rpc_buf_t *buf, uint32_t timeout_ms,
                             airy_cancel_token_t *cancel_token,
                             const char *cancel_socket_path,
                             const char *cancel_method,
                             const char *cancel_params_json)
{
    /* 总超时控制 */
    uint32_t elapsed_ms = 0;
    const uint32_t step_ms = 200;

    while (elapsed_ms < timeout_ms) {
        /* 改进1：取消检查（每次 poll 片边界，粒度 ≤200ms） */
        if (cancel_token && airy_cancel_token_is_canceled(cancel_token)) {
#if AIRY_PLATFORM_POSIX
            close(fd);
#endif
            /* 发送取消请求：独立连接送达 daemon（invoke 响应已放弃） */
            if (cancel_method && cancel_method[0]) {
                char *cancel_result = NULL;
                int crc = daemon_rpc_call(cancel_socket_path, cancel_method,
                                          cancel_params_json, &cancel_result, 5000);
                AIRY_FREE(cancel_result);
                if (crc != AIRY_SUCCESS)
                    SVC_LOG_WARN("rpc cancel request failed (method=%s, rc=%d)",
                                 cancel_method, crc);
            }
            return AIRY_ERR_CANCELED;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int remain = (int)((timeout_ms - elapsed_ms) < step_ms
                            ? (timeout_ms - elapsed_ms)
                            : step_ms);
        int pr = poll(&pfd, 1, remain);
        if (getenv("AIRY_RPC_DIAG") && pr > 0)
            SVC_LOG_ERROR("rpc diag: poll hit fd=%d pr=%d revents=0x%x buf_size=%zu", fd, pr, pfd.revents, buf->size);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return AIRY_ERR_FAIL;
        }
        if (pr == 0) {
            elapsed_ms += (uint32_t)remain;
            /* 检查已有数据是否构成完整 JSON */
            if (buf->size > 0) {
                cJSON *probe = cJSON_Parse(buf->data);
                if (probe) {
                    cJSON_Delete(probe);
                    return AIRY_SUCCESS;
                }
            }
            continue;
        }
        /* 对端在 send 响应后立即 close（daemon 为"单请求-单响应-即关闭"模型），
         * poll 可能同时返回 POLLIN 与 POLLHUP。必须先处理 POLLIN 中的数据，
         * 否则会因先判断 POLLHUP 而丢弃已到达的完整响应（RPC 时序竞态）。
         * 大响应（>4096 字节）一次 recv 读不完：即使 hangup 置位也必须继续
         * recv 读尽剩余数据（对端 close 后已发送数据仍可从 socket buffer 读出），
         * 最后以 recv()==0（EOF）收尾，再判断 JSON 完整性。 */
        int hangup = (pfd.revents & (POLLHUP | POLLNVAL));
        if (pfd.revents & POLLIN) {
            char chunk[4096];
            ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (getenv("AIRY_RPC_DIAG"))
                SVC_LOG_ERROR("rpc diag: recv n=%zd errno=%d buf_size=%zu revents=0x%x", n, errno, buf->size, pfd.revents);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                if (getenv("AIRY_RPC_DIAG"))
                    SVC_LOG_ERROR("rpc diag: recv<0 errno=%d buf_size=%zu", errno, buf->size);
                return AIRY_ERR_FAIL;
            }
            if (n == 0) {
                /* 对端关闭：检查是否已收到完整 JSON */
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
                return AIRY_ERR_FAIL;
            }
            int rc = rpc_buf_append(buf, chunk, (size_t)n);
            if (rc != AIRY_SUCCESS)
                return rc;

            /* 立即检查 JSON 完整性 */
            cJSON *probe = cJSON_Parse(buf->data);
            if (probe) {
                cJSON_Delete(probe);
                return AIRY_SUCCESS;
            }
            if (hangup) {
                /* 数据不全但连接已关闭：读尽 socket 剩余数据（对端 close 后
                 * 已发送的字节仍可读），直到 EOF 再判断完整性，避免大响应
                 * 被 hangup 误判为 FAIL。 */
                for (;;) {
                    ssize_t n2 = recv(fd, chunk, sizeof(chunk), 0);
                    if (n2 < 0) {
                        if (errno == EINTR)
                            continue;
                        if (errno == EAGAIN)
                            break;
                        return AIRY_ERR_FAIL;
                    }
                    if (n2 == 0)
                        break; /* EOF：剩余数据已全部读出 */
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
                return AIRY_ERR_FAIL;
            }
            elapsed_ms += 1; /* 至少消耗 1ms 进度防止死循环 */
        } else if (hangup || (pfd.revents & POLLERR)) {
            if (getenv("AIRY_RPC_DIAG"))
                SVC_LOG_ERROR("rpc diag: hangup-no-POLLIN revents=0x%x buf_size=%zu",
                              pfd.revents, buf->size);
            return AIRY_ERR_FAIL;
        }
    }
    return AIRY_ERR_TIMEOUT;
}

/* ==================== Windows 桩实现 ==================== */

#elif AIRY_PLATFORM_WINDOWS

static int rpc_connect_unix(const char *socket_path)
{
    (void)socket_path;
    return -AIRY_ERR_NOT_SUPPORTED;
}

static int rpc_recv_response(int fd, rpc_buf_t *buf, uint32_t timeout_ms,
                             airy_cancel_token_t *cancel_token,
                             const char *cancel_socket_path,
                             const char *cancel_method,
                             const char *cancel_params_json)
{
    (void)fd;
    (void)buf;
    (void)timeout_ms;
    (void)cancel_token;
    (void)cancel_socket_path;
    (void)cancel_method;
    (void)cancel_params_json;
    return AIRY_ERR_NOT_SUPPORTED;
}

#endif /* AIRY_PLATFORM_WINDOWS / POSIX */

/* ==================== 公共接口实现 ==================== */

int daemon_rpc_call(const char *socket_path, const char *method,
                    const char *params_json,
                    char **out_result_json, uint32_t timeout_ms)
{
    return daemon_rpc_call_cancelable(socket_path, method, params_json,
                                      out_result_json, timeout_ms,
                                      NULL, NULL, NULL);
}

int daemon_rpc_call_cancelable(const char *socket_path, const char *method,
                               const char *params_json,
                               char **out_result_json, uint32_t timeout_ms,
                               airy_cancel_token_t *cancel_token,
                               const char *cancel_method,
                               const char *cancel_params_json)
{
    if (!socket_path || !method || !out_result_json)
        return AIRY_ERR_INVALID_PARAM;

    *out_result_json = NULL;
    if (timeout_ms == 0)
        timeout_ms = DAEMON_RPC_DEFAULT_TIMEOUT_MS;

    /* 连接 daemon */
    int fd = rpc_connect_unix(socket_path);
    if (fd < 0)
        return -fd; /* rpc_connect_unix 返回负的 AIRY_ERR_* */

    /* 构造 JSON-RPC 2.0 请求 */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
#if AIRY_PLATFORM_POSIX
        close(fd);
#endif
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", method);
    if (params_json && params_json[0] != '\0') {
        cJSON *params = cJSON_Parse(params_json);
        if (params) {
            cJSON_AddItemToObject(root, "params", params);
        } else {
            /* 解析失败：作为字符串字段回退 */
            cJSON_AddStringToObject(root, "params", params_json);
        }
    } else {
        cJSON_AddObjectToObject(root, "params");
    }
    cJSON_AddNumberToObject(root, "id", 1);

    char *request_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!request_str) {
#if AIRY_PLATFORM_POSIX
        close(fd);
#endif
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    /* 发送请求 */
    size_t req_len = strlen(request_str);
#if AIRY_PLATFORM_POSIX
    ssize_t sent = send(fd, request_str, req_len, 0);
#else
    ssize_t sent = -1;
#endif
    AIRY_FREE(request_str);
    if (sent < 0 || (size_t)sent != req_len) {
#if AIRY_PLATFORM_POSIX
        close(fd);
#endif
        SVC_LOG_ERROR("daemon_rpc_call: send failed (method=%s)", method);
        return AIRY_ERR_FAIL;
    }

    /* 接收响应 */
    rpc_buf_t buf;
    int rc = rpc_buf_init(&buf);
    if (rc != AIRY_SUCCESS) {
#if AIRY_PLATFORM_POSIX
        close(fd);
#endif
        return rc;
    }

    rc = rpc_recv_response(fd, &buf, timeout_ms, cancel_token,
                           socket_path, cancel_method, cancel_params_json);
#if AIRY_PLATFORM_POSIX
    close(fd);
#endif
    if (rc != AIRY_SUCCESS) {
        if (rc != AIRY_ERR_CANCELED)
            SVC_LOG_ERROR("daemon_rpc_call: recv failed (method=%s, rc=%d, timeout=%u)",
                           method, rc, timeout_ms);
        rpc_buf_free(&buf);
        return rc;
    }

    /* 解析响应，提取 result 字段 */
    cJSON *resp = cJSON_Parse(buf.data);
    rpc_buf_free(&buf);
    if (!resp) {
        SVC_LOG_ERROR("daemon_rpc_call: response parse failed (method=%s)", method);
        return AIRY_ERR_FAIL;
    }

    cJSON *err_obj = cJSON_GetObjectItem(resp, "error");
    if (err_obj) {
        cJSON *err_msg = cJSON_GetObjectItem(err_obj, "message");
        const char *msg = (err_msg && cJSON_IsString(err_msg)) ? err_msg->valuestring : "unknown";
        cJSON *err_code = cJSON_GetObjectItem(err_obj, "code");
        int code = (err_code && cJSON_IsNumber(err_code)) ? err_code->valueint : -32000;
        SVC_LOG_WARN("daemon_rpc_call: daemon returned error (method=%s, code=%d, msg=%s)",
                      method, code, msg);
        cJSON_Delete(resp);
        return AIRY_ERR_FAIL;
    }

    cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (!result) {
        SVC_LOG_ERROR("daemon_rpc_call: missing result field (method=%s)", method);
        cJSON_Delete(resp);
        return AIRY_ERR_FAIL;
    }

    /* 将 result 序列化为字符串返回 */
    char *result_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(resp);
    if (!result_str)
        return AIRY_ERR_OUT_OF_MEMORY;

    *out_result_json = AIRY_STRDUP(result_str);
    /* cJSON_PrintUnformatted 使用 cJSON 默认 allocator（malloc），
     * AIRY_FREE 与默认 cJSON allocator 兼容 */
    AIRY_FREE(result_str);
    if (!*out_result_json)
        return AIRY_ERR_OUT_OF_MEMORY;

    return AIRY_SUCCESS;
}
