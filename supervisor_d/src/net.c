// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file net.c
 * @brief 端点机制件：跨平台 sock 可达性探测 + Windows TCP 回环端点的
 *        唯一装配权威（V13.5 零依赖硬约束下的模块内 SSoT）。
 *
 * 机制/策略分离：本件只提供「端点 → 可连接 fd / 可达判据」的机制，
 * 上层 ctrl.c（控制口装配）与 proc.c（假死探测）只持 fd 与字面量，
 * 不再各自复制 WSA 初始化与端点裁剪。
 *
 * 零依赖：本 target 禁止链接任何项目内库（link-whitelist 为空），故
 * WSA 初始化必须自持，不得委托 commons 的 airy_network_init()。
 *
 * POSIX：UDS（AF_UNIX，路径端点）；Windows：TCP 回环（host:port 字面量）。
 */

#include "sup_net.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

/* ---- 端点字面量裁剪：host:port → 两段（跨平台，可单测） ---- */
int sup_ep_split(const char *ep, char *host, size_t hsz, char *port, size_t psz)
{
    const char *colon;

    if (!ep || !host || !port || hsz == 0 || psz == 0)
        return -1;
    colon = strrchr(ep, ':');
    if (!colon || colon == ep || (size_t)(colon - ep) >= hsz)
        return -1;
    /* %.*s 截取 host 段：边界与终止由 snprintf 保证（BAN-154） */
    snprintf(host, hsz, "%.*s", (int)(colon - ep), ep);
    snprintf(port, psz, "%s", colon + 1);
    return 0;
}

/* ---- 非阻塞 connect + select 超时机制件：成功(0)/失败(-1) ---- */
static int sup_conn_wait(int fd, const struct sockaddr *sa, socklen_t salen,
                         int timeout_ms)
{
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
    if (connect(fd, sa, salen) == 0)
        return 0;
#ifndef _WIN32
    if (errno != EINPROGRESS)
        return -1;
#endif
    {
        fd_set wset;
        struct timeval tv;
        int err = 0;
        socklen_t elen = sizeof(err);

        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(fd + 1, NULL, &wset, NULL, &tv) <= 0)
            return -1;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) != 0 ||
            err != 0)
            return -1;
    }
    return 0;
}

#ifdef _WIN32
/* ---- WSA 初始化（幂等）：Windows 专有，POSIX 无需且不暴露 ---- */
static int sup_net_up(void)
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

/* ---- 端点解析权威：host:port → addrinfo（passive 决定 AI_PASSIVE） ---- */
static int win_resolve(const char *ep, int passive, struct addrinfo **res)
{
    char host[128], port[16];
    struct addrinfo hints;

    if (sup_net_up() != 0)
        return -1;
    if (sup_ep_split(ep, host, sizeof(host), port, sizeof(port)) != 0)
        return -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    if (getaddrinfo(host, port, &hints, res) != 0 || !*res)
        return -1;
    return 0;
}

/* ---- TCP 连接装配：ep → 已连接 fd（timeout_ms < 0 用阻塞 connect） ---- */
int sup_tcp_conn(const char *ep, int timeout_ms)
{
    struct addrinfo *res = NULL;
    int fd, rc;

    if (win_resolve(ep, 0, &res) != 0)
        return -1;
    fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    if (timeout_ms < 0)
        rc = connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen);
    else
        rc = sup_conn_wait(fd, res->ai_addr, (socklen_t)res->ai_addrlen,
                           timeout_ms);
    freeaddrinfo(res);
    if (rc != 0) {
        closesocket(fd);
        return -1;
    }
    return fd;
}

/* ---- TCP 监听装配：ep → 已 listen fd（SO_REUSEADDR + backlog 4） ---- */
int sup_tcp_bind(const char *ep)
{
    struct addrinfo *res = NULL;
    BOOL reuse = TRUE;
    int fd;

    if (win_resolve(ep, 1, &res) != 0)
        return -1;
    fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));
    if (bind(fd, res->ai_addr, (socklen_t)res->ai_addrlen) != 0 ||
        listen(fd, 4) != 0) {
        closesocket(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int probe_tcp(const char *ep)
{
    int fd = sup_tcp_conn(ep, SUP_PROBE_TIMEOUT_MS);

    if (fd < 0)
        return -1;
    closesocket(fd);
    return 0;
}
#else
static int probe_unix(const char *path)
{
    struct sockaddr_un sa;
    int fd, rc;

    if (strlen(path) >= sizeof(sa.sun_path))
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    rc = sup_conn_wait(fd, (const struct sockaddr *)&sa, sizeof(sa),
                       SUP_PROBE_TIMEOUT_MS);
    close(fd);
    return rc;
}
#endif

/* ---- sock 可达性判据：无登记端点返回 -1（调用方须走进程存活腿） ---- */
int sup_probe_sock(const char *sock)
{
    if (!sock || sock[0] == '\0')
        return -1;
#ifdef _WIN32
    return probe_tcp(sock);
#else
    return probe_unix(sock);
#endif
}
