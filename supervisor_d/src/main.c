// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file main.c
 * @brief supervisor_d 入口：常驻调谐主循环 + 一次性控制口客户端。
 *
 * 用法：
 *   supervisor_d                 常驻监管（launcher 唯一启动路径：nohup &）
 *   supervisor_d activate <name> 单向激活请求（FAILED 复位运维兜底）
 *   supervisor_d stop            收摊归一（supervisor.shutdown）
 *   supervisor_d status          health_check 摘要
 *   supervisor_d --help
 *
 * 生命周期（V13.4/V13.5）：pid 文件防重复启动（自持，无二级监管者）；
 * 主循环 reconcile→serve 每 tick 一轮；SIGTERM/SIGINT 与
 * supervisor.shutdown 同路径收摊；退出清 pid 文件与 UDS 残留。
 */

#include "supervisor_d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

static volatile sig_atomic_t g_stop = 0;

#ifndef _WIN32
static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
}
#endif

static void mkdir_p(const char *path)
{
    char tmp[SUP_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
#ifdef _WIN32
    for (char *q = tmp + 1; *q; q++) {
        if (*q == '\\') {
            *q = '\0';
            CreateDirectoryA(tmp, NULL);
            *q = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
#else
    for (char *q = tmp + 1; *q; q++) {
        if (*q == '/') {
            *q = '\0';
            mkdir(tmp, 0755);
            *q = '/';
        }
    }
    mkdir(tmp, 0755);
#endif
}

static void ensure_dirs(const sup_ctx_t *ctx)
{
    char d[SUP_PATH_MAX];
    snprintf(d, sizeof(d), "%s/config", ctx->airy_home);
    mkdir_p(d);
    snprintf(d, sizeof(d), "%s", ctx->runtime_dir);
    mkdir_p(d);
    snprintf(d, sizeof(d), "%s", ctx->log_dir);
    mkdir_p(d);
}

static int pid_alive(long pid)
{
    if (pid <= 0)
        return 0;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           (DWORD)pid);
    if (!h)
        return 0;
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

int sup_pidfile_write(const sup_ctx_t *ctx)
{
    char pf[SUP_PATH_MAX];
    snprintf(pf, sizeof(pf), "%s/supervisor.pid", ctx->runtime_dir);
    FILE *f = fopen(pf, "r");
    if (f) {
        char line[64];
        if (fgets(line, sizeof(line), f)) {
            char *end = NULL;
            long old = strtol(line, &end, 10);
            if (end != line && pid_alive(old)) {
                fclose(f);
                return -1; /* 已有实例（唯一启动路径判据） */
            }
        }
        fclose(f);
    }
    f = fopen(pf, "w");
    if (!f)
        return -1;
    fprintf(f, "%ld\n", (long)getpid());
    fclose(f);
    return 0;
}

void sup_pidfile_clear(const sup_ctx_t *ctx)
{
    char pf[SUP_PATH_MAX];
    snprintf(pf, sizeof(pf), "%s/supervisor.pid", ctx->runtime_dir);
    remove(pf);
}

/* 一次性控制口客户端：连 ctrl_ep 发单行请求，打印响应 */
static int client_run(const sup_ctx_t *ctx, const char *req)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;
    char host[128], port[16];
    const char *colon = strrchr(ctx->ctrl_ep, ':');
    if (!colon || colon == ctx->ctrl_ep ||
        (size_t)(colon - ctx->ctrl_ep) >= sizeof(host))
        return -1;
    size_t hl = (size_t)(colon - ctx->ctrl_ep);
    snprintf(host, sizeof(host), "%.*s", (int)hl, ctx->ctrl_ep);
    snprintf(port, sizeof(port), "%s", colon + 1);
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
#ifdef _WIN32
    closesocket(fd);
    WSACleanup();
#else
    close(fd);
#endif
    return 0;
}

static int usage(void)
{
    fputs("usage: supervisor_d [activate <name>|stop|status|--help]\n", stderr);
    return 2;
}

int main(int argc, char **argv)
{
    sup_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (sup_decl_load(&ctx) != 0) {
        sup_log("ERROR", "decl load failed");
        return 1;
    }

    if (argc >= 2) {
        char req[SUP_NAME_MAX + 128];
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
            return usage();
        if (strcmp(argv[1], "activate") == 0 && argc == 3) {
            snprintf(req, sizeof(req),
                     "{\"method\":\"supervisor.activate\",\"params\":{\"name\":"
                     "\"%s\"},\"id\":1}\n",
                     argv[2]);
            return client_run(&ctx, req) == 0 ? 0 : 1;
        }
        if (strcmp(argv[1], "stop") == 0 && argc == 2) {
            snprintf(req, sizeof(req),
                     "{\"method\":\"supervisor.shutdown\",\"id\":1}\n");
            return client_run(&ctx, req) == 0 ? 0 : 1;
        }
        if (strcmp(argv[1], "status") == 0 && argc == 2) {
            snprintf(req, sizeof(req),
                     "{\"method\":\"health_check\",\"id\":1}\n");
            return client_run(&ctx, req) == 0 ? 0 : 1;
        }
        return usage();
    }

#ifdef _WIN32
    if (sup_proc_job_init() != 0)
        sup_log("WARN", "job object init failed, orphan guard disabled");
#endif
    ensure_dirs(&ctx);
    if (sup_pidfile_write(&ctx) != 0) {
        sup_log("ERROR", "another supervisor running (pid file)");
        return 1;
    }
#ifndef _WIN32
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL); /* reap 层 waitpid 依赖默认处置 */
#endif

    int ncore = 0, naux = 0;
    for (int i = 0; i < ctx.count; i++) {
        if (ctx.procs[i].role == SUP_ROLE_CORE)
            ncore++;
        else
            naux++;
    }
    int lfd = sup_ctrl_listen(&ctx);
    if (lfd < 0) {
        sup_log("ERROR", "ctrl listen failed: %s", ctx.ctrl_ep);
        sup_pidfile_clear(&ctx);
        return 1;
    }
    sup_log("INFO", "up: home=%s %d daemons (core=%d aux=%d) tick=%ldms ep=%s",
            ctx.airy_home, ctx.count, ncore, naux, ctx.tick_ms, ctx.ctrl_ep);

    while (!g_stop && !ctx.shutdown) {
        sup_reconcile(&ctx);
        if (g_stop || ctx.shutdown)
            break;
        sup_ctrl_serve(&ctx, lfd, (int)ctx.tick_ms);
    }
    sup_shutdown_all(&ctx);
    sup_ctrl_close(lfd);
#ifndef _WIN32
    unlink(ctx.ctrl_ep);
#endif
    sup_pidfile_clear(&ctx);
    return 0;
}
