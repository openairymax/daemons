// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file ctrl.c
 * @brief 控制口：POSIX UDS / Windows TCP 回环 + 极简 JSON-RPC（自持实现）。
 *
 * 方法面（V13.2 / V13.4）：
 *   supervisor.activate  {"name":"monit_d"}   AUX 按需拉起（幂等）
 *   supervisor.shutdown  {}                   收摊归一（主循环受理）
 *   health_check         {}                   进程表状态摘要
 * 请求/响应均为单行 JSON，串行单连接处理（控制面低频，无并发需求）。
 * UDS 文件权限 0600，仅本机同用户可达；TCP 腿仅绑定 127.0.0.1。
 */

#include "supervisor_d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#define SUP_REQ_MAX 2048

static const char *state_str(sup_state_t st)
{
    switch (st) {
    case SUP_ST_STARTING: return "starting";
    case SUP_ST_RUNNING:  return "running";
    case SUP_ST_BACKOFF:  return "backoff";
    case SUP_ST_FAILED:   return "failed";
    default:              return "stopped";
    }
}

#ifdef _WIN32
static int wsa_up(void)
{
    static WSADATA wsa;
    static int done = 0;
    if (!done) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return -1;
        done = 1;
    }
    return 0;
}
#endif

int sup_ctrl_listen(const sup_ctx_t *ctx)
{
#ifdef _WIN32
    if (wsa_up() != 0)
        return -1;
    char host[128], port[16];
    const char *colon = strrchr(ctx->ctrl_ep, ':');
    if (!colon || colon == ctx->ctrl_ep || (size_t)(colon - ctx->ctrl_ep) >= sizeof(host))
        return -1;
    /* %.*s 截取 host 段：边界与终止由 snprintf 保证（BAN-154） */
    snprintf(host, sizeof(host), "%.*s", (int)(colon - ctx->ctrl_ep), ctx->ctrl_ep);
    snprintf(port, sizeof(port), "%s", colon + 1);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return -1;
    int fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    BOOL reuse = TRUE;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
    if (bind(fd, res->ai_addr, (socklen_t)res->ai_addrlen) != 0 ||
        listen(fd, 4) != 0) {
        closesocket(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
#else
    struct sockaddr_un sa;
    if (strlen(ctx->ctrl_ep) >= sizeof(sa.sun_path))
        return -1;
    unlink(ctx->ctrl_ep); /* 幂等补拉：清 stale socket */
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", ctx->ctrl_ep);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (bind(fd, (const struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        chmod(ctx->ctrl_ep, 0600) != 0 || listen(fd, 4) != 0) {
        close(fd);
        unlink(ctx->ctrl_ep);
        return -1;
    }
    return fd;
#endif
}

void sup_ctrl_close(int fd)
{
    if (fd < 0)
        return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        int n = (int)send(fd, buf + off, (int)(len - off), 0);
#else
        ssize_t n = write(fd, buf + off, len - off);
#endif
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

int sup_json_field(const char *json, const char *key, char *out, size_t out_sz)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(json, pat);
    if (!k)
        return -1;
    const char *c = k + strlen(pat);
    while (*c == ' ' || *c == '\t')
        c++;
    if (*c != ':')
        return -1;
    c++;
    while (*c == ' ' || *c == '\t')
        c++;
    if (*c != '"')
        return -1;
    c++;
    size_t i = 0;
    while (*c && *c != '"') {
        if (*c == '\\' && c[1])
            c++; /* 跳过转义，保留后字符 */
        if (i + 1 < out_sz)
            out[i++] = *c;
        c++;
    }
    out[i] = '\0';
    return *c == '"' ? 0 : -1;
}

static void dispatch(sup_ctx_t *ctx, int fd, const char *req)
{
    char method[64], name[SUP_NAME_MAX], resp[SUP_MAX_DAEMONS * 96 + 128];
    long long id = 0;
    const char *ids = strstr(req, "\"id\"");
    if (ids)
        id = atoll(strchr(ids, ':') ? strchr(ids, ':') + 1 : "0");

    if (sup_json_field(req, "method", method, sizeof(method)) != 0) {
        snprintf(resp, sizeof(resp), "{\"error\":\"missing method\",\"id\":%lld}\n", id);
        send_all(fd, resp, strlen(resp));
        return;
    }
    if (strcmp(method, "supervisor.activate") == 0) {
        if (sup_json_field(req, "name", name, sizeof(name)) != 0) {
            snprintf(resp, sizeof(resp), "{\"error\":\"missing name\",\"id\":%lld}\n", id);
        } else if (sup_activate(ctx, name) == 0) {
            snprintf(resp, sizeof(resp), "{\"result\":\"ok\",\"id\":%lld}\n", id);
            sup_log("INFO", "activate %s via ctrl", name);
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"error\":\"activate failed\",\"id\":%lld}\n", id);
        }
    } else if (strcmp(method, "supervisor.shutdown") == 0) {
        ctx->shutdown = 1; /* 主循环受理，串行安全 */
        snprintf(resp, sizeof(resp), "{\"result\":\"ok\",\"id\":%lld}\n", id);
    } else if (strcmp(method, "health_check") == 0) {
        size_t off = (size_t)snprintf(resp, sizeof(resp),
                                      "{\"result\":\"ok\",\"daemons\":[");
        for (int i = 0; i < ctx->count && off < sizeof(resp); i++) {
            const sup_proc_t *p = &ctx->procs[i];
            int n = snprintf(resp + off, sizeof(resp) - off,
                             "%s{\"name\":\"%s\",\"state\":\"%s\",\"fails\":%d}",
                             i ? "," : "", p->name, state_str(p->state),
                             p->fail_count);
            if (n < 0)
                break;
            off += (size_t)n;
        }
        snprintf(resp + off, sizeof(resp) - off, "],\"id\":%lld}\n", id);
    } else {
        snprintf(resp, sizeof(resp), "{\"error\":\"unknown method\",\"id\":%lld}\n", id);
    }
    send_all(fd, resp, strlen(resp));
}

void sup_ctrl_serve(sup_ctx_t *ctx, int listen_fd, int timeout_ms)
{
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(listen_fd, &rset);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (select(listen_fd + 1, &rset, NULL, NULL, &tv) <= 0)
        return;
#ifdef _WIN32
    int fd = (int)accept(listen_fd, NULL, NULL);
#else
    int fd = accept(listen_fd, NULL, NULL);
#endif
    if (fd < 0)
        return;
    char req[SUP_REQ_MAX];
    size_t off = 0;
    for (;;) {
        char chunk[512];
#ifdef _WIN32
        int n = (int)recv(fd, chunk, (int)sizeof(chunk), 0);
#else
        ssize_t n = read(fd, chunk, sizeof(chunk));
#endif
        if (n <= 0)
            break;
        if (off + (size_t)n >= sizeof(req)) {
            off = 0; /* 超长请求丢弃 */
            break;
        }
        /* %.*s 追加：容量与终止由 snprintf 保证（BAN-154） */
        snprintf(req + off, sizeof(req) - off, "%.*s", (int)n, chunk);
        off += (size_t)n;
        if (memchr(req, '\n', off) || memchr(req, '}', off))
            break; /* 单行协议：读到行尾即处理 */
    }
    if (off > 0)
        dispatch(ctx, fd, req);
    sup_ctrl_close(fd);
}
