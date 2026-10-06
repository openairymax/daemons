// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file ctrl.c
 * @brief 控制口：POSIX UDS / Windows TCP 回环 + 极简 JSON-RPC（自持实现）。
 *
 * 服务面（V13.2 / V13.4）：
 *   supervisor.activate  {"name":"monit_d"}   AUX 按需拉起（幂等）
 *   supervisor.shutdown  {}                   收摊归一（主循环受理）
 *   health_check         {}                   进程表状态摘要
 * 请求/响应均为单行 JSON，串行单连接处理（控制面低频，无并发需求）。
 * UDS 文件权限 0600，仅本机同用户可达；TCP 腿仅绑定 127.0.0.1。
 * 客户端面（launcher 唯一启动契约）：activate/stop/status 一次性连接。
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

static int ep_split(const char *ep, char *host, size_t hsz, char *port,
                    size_t psz)
{
    const char *colon = strrchr(ep, ':');
    if (!colon || colon == ep || (size_t)(colon - ep) >= hsz)
        return -1;
    /* %.*s 截取 host 段：边界与终止由 snprintf 保证（BAN-154） */
    snprintf(host, hsz, "%.*s", (int)(colon - ep), ep);
    snprintf(port, psz, "%s", colon + 1);
    return 0;
}
#endif

int sup_ctrl_listen(const sup_ctx_t *ctx)
{
#ifdef _WIN32
    if (wsa_up() != 0)
        return -1;
    char host[128], port[16];
    if (ep_split(ctx->ctrl_ep, host, sizeof(host), port, sizeof(port)) != 0)
        return -1;

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

/* 顶层键定位机制件：只认「其后紧随 ':'」的 "key" 匹配，跳过值域内
 * 出现的同名字面量（如某字符串值里内嵌 \"method\":\"...\"），杜绝把
 * 值当成键而误配。返回值为越过冒号与空白后的首字符。 */
static const char *sup_find_key(const char *json, const char *key)
{
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (!json || !key || n <= 0 || (size_t)n >= sizeof(pat))
        return NULL;
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *c = p + (size_t)n;
        while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')
            c++;
        if (*c != ':') {
            p = c;
            continue;
        }
        c++;
        while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')
            c++;
        return c;
    }
    return NULL;
}

int sup_json_field(const char *json, const char *key, char *out, size_t out_sz)
{
    if (!json || !key || !out || out_sz == 0)
        return -1;
    const char *c = sup_find_key(json, key);
    if (!c || *c != '"')
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
    const char *idp = sup_find_key(req, "id");
    if (idp)
        id = atoll(idp);

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

/* 一次性控制口客户端：连 ctrl_ep 发单行请求，打印响应 */
static int client_run(const sup_ctx_t *ctx, const char *req)
{
#ifdef _WIN32
    if (wsa_up() != 0)
        return -1;
    char host[128], port[16];
    if (ep_split(ctx->ctrl_ep, host, sizeof(host), port, sizeof(port)) != 0)
        return -1;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return -1;
    int fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    int rc = fd >= 0 ? connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) : -1;
    freeaddrinfo(res);
    if (rc != 0)
        return -1;
#else
    struct sockaddr_un sa;
    if (strlen(ctx->ctrl_ep) >= sizeof(sa.sun_path))
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", ctx->ctrl_ep);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (const struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
#endif
    size_t len = strlen(req);
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        int n = (int)send(fd, req + off, (int)(len - off), 0);
#else
        ssize_t n = write(fd, req + off, len - off);
#endif
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    char buf[4096];
    ssize_t n;
#ifdef _WIN32
    while ((n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
#else
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
#endif
        buf[n] = '\0';
        fputs(buf, stdout);
        if (memchr(buf, '\n', (size_t)n))
            break;
    }
    sup_ctrl_close(fd);
    return 0;
}

static int usage(void)
{
    fputs("usage: supervisor_d [activate <name>|stop|status|--help]\n", stderr);
    return 2;
}

/* CLI 子命令分派（launcher 唯一启动契约）：仅 argc >= 2 时进入 */
int sup_ctrl_client(sup_ctx_t *ctx, int argc, char **argv)
{
    char req[SUP_NAME_MAX + 128];
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
        return usage();
    if (strcmp(argv[1], "activate") == 0 && argc == 3) {
        snprintf(req, sizeof(req),
                 "{\"method\":\"supervisor.activate\",\"params\":{\"name\":"
                 "\"%s\"},\"id\":1}\n",
                 argv[2]);
        return client_run(ctx, req) == 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "stop") == 0 && argc == 2) {
        snprintf(req, sizeof(req),
                 "{\"method\":\"supervisor.shutdown\",\"id\":1}\n");
        return client_run(ctx, req) == 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "status") == 0 && argc == 2) {
        snprintf(req, sizeof(req),
                 "{\"method\":\"health_check\",\"id\":1}\n");
        return client_run(ctx, req) == 0 ? 0 : 1;
    }
    return usage();
}
