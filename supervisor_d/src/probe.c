// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file probe.c
 * @brief 健康探测层：sock 可达性（非阻塞 connect，500ms 超时）+ 假死门控。
 *
 * 双层判定之第二层：进程存活由 proc.c reap 层判定；本层对已存活进程
 * 探测服务端点可达性，STARTING→RUNNING 迁移以此佐证（探测通过时
 * fail_count 归零），连续 liveness_ticks 个周期不可达判定假死并强杀，
 * 交由 reap 记录死因进入退避。sock 为空者（Windows 无登记端点的
 * maths）仅进程存活判定，跳过本层。
 */

#include "supervisor_d.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#define SUP_PROBE_TIMEOUT_MS 500

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

/* 非阻塞 connect + select 超时，成功(0)/失败(-1) */
static int connect_wait(int fd, const struct sockaddr *sa, socklen_t salen)
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
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv = {SUP_PROBE_TIMEOUT_MS / 1000,
                         (SUP_PROBE_TIMEOUT_MS % 1000) * 1000};
    if (select(fd + 1, NULL, &wset, NULL, &tv) <= 0)
        return -1;
    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) != 0 || err != 0)
        return -1;
    return 0;
}

#ifdef _WIN32
static int probe_tcp(const char *ep)
{
    if (wsa_up() != 0)
        return -1;
    char host[128], port[16];
    const char *colon = strrchr(ep, ':');
    if (!colon || colon == ep || (size_t)(colon - ep) >= sizeof(host))
        return -1;
    /* %.*s 截取 host 段：边界与终止由 snprintf 保证（BAN-154） */
    snprintf(host, sizeof(host), "%.*s", (int)(colon - ep), ep);
    snprintf(port, sizeof(port), "%s", colon + 1);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return -1;
    int fd = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    int rc = connect_wait(fd, res->ai_addr, (socklen_t)res->ai_addrlen);
    freeaddrinfo(res);
    closesocket(fd);
    return rc;
}
#else
static int probe_unix(const char *path)
{
    struct sockaddr_un sa;
    if (strlen(path) >= sizeof(sa.sun_path))
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int rc = connect_wait(fd, (const struct sockaddr *)&sa, sizeof(sa));
    close(fd);
    return rc;
}
#endif

int sup_probe_sock(const sup_proc_t *p)
{
    if (p->sock[0] == '\0')
        return -1; /* 无登记端点，调用方须走进程存活腿 */
#ifdef _WIN32
    return probe_tcp(p->sock);
#else
    return probe_unix(p->sock);
#endif
}

void sup_health_tick(sup_ctx_t *ctx, sup_proc_t *p)
{
    if (p->pid == SUP_PID_INVALID)
        return; /* 死亡由 reap 层收割 */
    if (p->sock[0] == '\0') { /* 仅进程存活腿（Windows maths） */
        if (p->state == SUP_ST_STARTING) {
            p->state = SUP_ST_RUNNING;
            p->fail_count = 0;
        }
        return;
    }
    if (sup_probe_sock(p) == 0) {
        if (p->state != SUP_ST_RUNNING)
            sup_log("INFO", "%s healthy (sock=%s)", p->name, p->sock);
        p->state = SUP_ST_RUNNING;
        p->dead_ticks = 0;
        p->fail_count = 0;
        return;
    }
    p->dead_ticks++;
    if (p->dead_ticks >= ctx->liveness_ticks) {
        sup_log("WARN", "%s liveness lost after %d ticks, kill (sock=%s)",
                p->name, p->dead_ticks, p->sock);
#ifdef _WIN32
        TerminateProcess(p->pid, 1);
#else
        kill(p->pid, SIGKILL);
#endif
        /* reap 下轮收割记录死因并转退避 */
    }
}
