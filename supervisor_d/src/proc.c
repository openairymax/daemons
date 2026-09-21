// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file proc.c
 * @brief 进程原语层：spawn / reap / 指数退避 / 调谐 / 按需激活 / 收摊。
 *
 * spawn：POSIX fork/exec + setsid + PR_SET_PDEATHSIG（防孤儿）+ 日志
 *        重定向到 $AIRY_HOME/logs/<name>.out；Windows CreateProcess
 *        (CREATE_SUSPENDED) + Job Object KILL_ON_JOB_CLOSE。
 * reap：waitpid(WNOHANG) / WaitForSingleObject(0) 收割死因写入
 *       last_death（V13.1 判据）；CORE 死进 BACKOFF，AUX 死回 STOPPED
 *       （仅 activate 复活）。
 * 调谐：sup_reconcile 每周期比对期望态与实际态；连败超限转 FAILED 并
 *       显式告警，不再自动重启（V13.3 防风暴）；复位 FAILED 的唯一
 *       入口是 sup_activate 显式激活。
 */

#include "supervisor_d.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

#define SUP_ARGV_MAX 64

#ifdef _WIN32
static HANDLE g_job = NULL;

int sup_proc_job_init(void)
{
    g_job = CreateJobObjectA(NULL, "airymaxrt-supervisor");
    if (!g_job)
        return -1;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof(li));
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li,
                                 sizeof(li))) {
        CloseHandle(g_job);
        g_job = NULL;
        return -1;
    }
    return 0;
}
#endif

long long sup_now_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* 墙钟毫秒：仅日志时间戳使用（调谐计时一律走 sup_now_ms 单调钟） */
static long long wall_ms(void)
{
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000ULL) - 11644473600000LL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void sup_log(const char *level, const char *fmt, ...)
{
    long long ms = wall_ms();
    time_t sec = (time_t)(ms / 1000);
    struct tm tmv;
    char tsbuf[32];
#ifdef _WIN32
    localtime_s(&tmv, &sec);
#else
    localtime_r(&sec, &tmv);
#endif
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S", &tmv);
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s.%03lld [%s] supervisor_d: ", tsbuf, ms % 1000, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

#ifdef _WIN32
static void open_win_log(sup_ctx_t *ctx, sup_proc_t *p, SECURITY_ATTRIBUTES *sa,
                         STARTUPINFOA *si)
{
    char out[SUP_PATH_MAX];
    snprintf(out, sizeof(out), "%s\\%s.out", ctx->log_dir, p->name);
    HANDLE h = CreateFileA(out, FILE_APPEND_DATA, FILE_SHARE_READ, sa,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    si->dwFlags = STARTF_USESTDHANDLES;
    si->hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si->hStdOutput = h;
    si->hStdError = h;
}

static int spawn_win(sup_ctx_t *ctx, sup_proc_t *p)
{
    char cmd[SUP_ARGS_MAX + SUP_PATH_MAX];
    if (p->args[0])
        snprintf(cmd, sizeof(cmd), "\"%s\" %s", p->bin, p->args);
    else
        snprintf(cmd, sizeof(cmd), "\"%s\"", p->bin);

    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    open_win_log(ctx, p, &sa, &si);

    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &si,
                        &pi)) {
        sup_log("ERROR", "spawn %s failed: %lu", p->name, GetLastError());
        return -1;
    }
    if (si.hStdOutput && si.dwFlags & STARTF_USESTDHANDLES)
        CloseHandle(si.hStdOutput);
    if (g_job && !AssignProcessToJobObject(g_job, pi.hProcess))
        sup_log("WARN", "%s job attach failed: %lu", p->name, GetLastError());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p->pid = pi.hProcess;
    sup_log("INFO", "spawn %s pid=%lu attempt=%d", p->name,
            (unsigned long)(uintptr_t)p->pid, p->fail_count + 1);
    return 0;
}
#else
static int spawn_posix(sup_ctx_t *ctx, sup_proc_t *p)
{
    char *argv[SUP_ARGV_MAX];
    int argc = 0;
    argv[argc++] = p->bin;
    char argbuf[SUP_ARGS_MAX];
    snprintf(argbuf, sizeof(argbuf), "%s", p->args);
    char *save = NULL;
    for (char *tok = strtok_r(argbuf, " \t", &save); tok;
         tok = strtok_r(NULL, " \t", &save)) {
        if (argc >= SUP_ARGV_MAX - 1) {
            sup_log("ERROR", "%s args too many (cap %d)", p->name, SUP_ARGV_MAX);
            return -1;
        }
        argv[argc++] = tok;
    }
    argv[argc] = NULL;

#ifdef __linux__
    /* PDEATHSIG race 检查仅 Linux 可用，ppid 须在 fork 前采样 */
    pid_t ppid = getpid();
#endif
    pid_t pid = fork();
    if (pid < 0) {
        sup_log("ERROR", "fork %s: %s", p->name, strerror(errno));
        return -1;
    }
    if (pid == 0) {
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGTERM); /* 防孤儿：supervisor 死则 TERM */
        if (getppid() != ppid)
            _exit(127); /* race：父进程已先死 */
#endif
        setsid();
        /* 撤销继承自 supervisor 的忽略处置，保 shutdown 信号语义 */
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        char out[SUP_PATH_MAX];
        snprintf(out, sizeof(out), "%s/%s.out", ctx->log_dir, p->name);
        int fd = open(out, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                close(fd);
        }
        execv(p->bin, argv);
        _exit(127);
    }
    p->pid = pid;
    sup_log("INFO", "spawn %s pid=%d attempt=%d", p->name, (int)p->pid,
            p->fail_count + 1);
    return 0;
}
#endif

int sup_proc_spawn(sup_ctx_t *ctx, sup_proc_t *p)
{
    if (p->pid != SUP_PID_INVALID)
        return 0;
    int rc;
#ifdef _WIN32
    rc = spawn_win(ctx, p);
#else
    rc = spawn_posix(ctx, p);
#endif
    if (rc != 0) {
        p->fail_count++;
        return -1;
    }
    p->state = SUP_ST_STARTING;
    p->dead_ticks = 0;
    return 0;
}

void sup_proc_reap(sup_ctx_t *ctx)
{
    for (int i = 0; i < ctx->count; i++) {
        sup_proc_t *p = &ctx->procs[i];
        if (p->pid == SUP_PID_INVALID)
            continue;
        char cause[96];
#ifdef _WIN32
        DWORD code = 0;
        if (WaitForSingleObject(p->pid, 0) != WAIT_OBJECT_0)
            continue;
        if (!GetExitCodeProcess(p->pid, &code))
            snprintf(cause, sizeof(cause), "reap-error=%lu", GetLastError());
        else
            snprintf(cause, sizeof(cause), "exit=%lu", (unsigned long)code);
        CloseHandle(p->pid);
#else
        int st = 0;
        pid_t r = waitpid(p->pid, &st, WNOHANG);
        if (r == 0)
            continue; /* 仍在运行 */
        if (r < 0) {
            if (errno == EINTR)
                continue;
            snprintf(cause, sizeof(cause), "wait-error=%d", errno);
        } else if (WIFEXITED(st)) {
            snprintf(cause, sizeof(cause), "exit=%d", WEXITSTATUS(st));
        } else if (WIFSIGNALED(st)) {
            snprintf(cause, sizeof(cause), "signal=%d", WTERMSIG(st));
        } else {
            snprintf(cause, sizeof(cause), "status=0x%x", st);
        }
#endif
        p->pid = SUP_PID_INVALID;
        p->fail_count++;
        snprintf(p->last_death, sizeof(p->last_death), "%s", cause);
        sup_log("WARN", "%s died: %s (fail=%d)", p->name, cause, p->fail_count);
        if (p->role == SUP_ROLE_CORE) {
            p->state = SUP_ST_BACKOFF;
            p->next_ms = sup_now_ms() + sup_backoff_ms(ctx, p);
        } else {
            p->state = SUP_ST_STOPPED; /* AUX 死不自动复活 */
        }
    }
}

long long sup_backoff_ms(const sup_ctx_t *ctx, const sup_proc_t *p)
{
    long long ms = ctx->backoff_base_ms;
    for (int i = 1; i < p->fail_count && ms < ctx->backoff_max_ms; i++)
        ms *= 2;
    return ms < ctx->backoff_max_ms ? ms : ctx->backoff_max_ms;
}

static void spawn_or_backoff(sup_ctx_t *ctx, sup_proc_t *p, long long now)
{
    if (sup_proc_spawn(ctx, p) == 0)
        return;
    p->state = SUP_ST_BACKOFF;
    p->next_ms = now + sup_backoff_ms(ctx, p);
}

void sup_reconcile(sup_ctx_t *ctx)
{
    long long now = sup_now_ms();
    sup_proc_reap(ctx);
    for (int i = 0; i < ctx->count; i++) {
        sup_proc_t *p = &ctx->procs[i];
        if (p->role != SUP_ROLE_CORE)
            continue; /* AUX 仅 activate 拉起 */
        switch (p->state) {
        case SUP_ST_STOPPED:
            spawn_or_backoff(ctx, p, now);
            break;
        case SUP_ST_BACKOFF:
            if (now >= p->next_ms) {
                if (p->fail_count >= ctx->max_attempts) {
                    p->state = SUP_ST_FAILED;
                    sup_log("ERROR",
                            "%s FAILED after %d attempts, manual activate required",
                            p->name, p->fail_count);
                } else {
                    spawn_or_backoff(ctx, p, now);
                }
            }
            break;
        case SUP_ST_STARTING:
        case SUP_ST_RUNNING:
            sup_health_tick(ctx, p);
            break;
        default:
            break; /* FAILED：等待显式 activate 复位 */
        }
    }
}

int sup_activate(sup_ctx_t *ctx, const char *name)
{
    int i = sup_proc_find(ctx, name);
    if (i < 0) {
        sup_log("WARN", "activate: unknown daemon '%s'", name ? name : "");
        return -1;
    }
    sup_proc_t *p = &ctx->procs[i];
    if (p->pid != SUP_PID_INVALID)
        return 0; /* 幂等：已在运行 */
    p->fail_count = 0; /* FAILED 复位唯一入口 */
    p->state = SUP_ST_STOPPED;
    long long now = sup_now_ms();
    spawn_or_backoff(ctx, p, now);
    return p->state == SUP_ST_STARTING ? 0 : -1;
}

void sup_shutdown_all(sup_ctx_t *ctx)
{
    sup_log("INFO", "shutdown: stopping %d daemons", ctx->count);
    for (int i = 0; i < ctx->count; i++) {
        sup_proc_t *p = &ctx->procs[i];
        if (p->pid == SUP_PID_INVALID)
            continue;
#ifdef _WIN32
        TerminateProcess(p->pid, 0); /* Windows 无优雅信号等价物 */
#else
        kill(p->pid, SIGTERM);
#endif
    }
    long long deadline = sup_now_ms() + SUP_TERM_GRACE_MS;
    for (;;) {
        int live = 0;
        for (int i = 0; i < ctx->count; i++) {
            sup_proc_t *p = &ctx->procs[i];
            if (p->pid == SUP_PID_INVALID)
                continue;
#ifdef _WIN32
            if (WaitForSingleObject(p->pid, 0) == WAIT_OBJECT_0) {
                CloseHandle(p->pid);
                p->pid = SUP_PID_INVALID;
            } else {
                live++;
            }
#else
            int st = 0;
            pid_t r = waitpid((pid_t)p->pid, &st, WNOHANG);
            if (r == (pid_t)p->pid || (r < 0 && errno == ECHILD))
                p->pid = SUP_PID_INVALID; /* 死透即收割，宽限提前结束 */
            else
                live++;
#endif
        }
        if (live == 0 || sup_now_ms() >= deadline)
            break;
#ifdef _WIN32
        Sleep(100);
#else
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
#endif
    }
    for (int i = 0; i < ctx->count; i++) {
        sup_proc_t *p = &ctx->procs[i];
        if (p->pid == SUP_PID_INVALID)
            continue;
        sup_log("WARN", "%s grace timeout, kill", p->name);
#ifdef _WIN32
        TerminateProcess(p->pid, 1);
        CloseHandle(p->pid);
        p->pid = SUP_PID_INVALID;
#else
        kill(p->pid, SIGKILL);
#endif
    }
#ifndef _WIN32
    for (int i = 0; i < ctx->count; i++) { /* 收割 KILL 尸体 */
        sup_proc_t *p = &ctx->procs[i];
        if (p->pid == SUP_PID_INVALID)
            continue;
        int st;
        while (waitpid(p->pid, &st, 0) < 0 && errno == EINTR)
            ;
        p->pid = SUP_PID_INVALID;
    }
#endif
    sup_log("INFO", "shutdown complete");
}
