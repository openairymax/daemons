/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_crash.c
 * @brief 守护进程致命信号现场落盘：崩溃归因的确定性证据。
 *
 * 进程被 SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT 终止时，内核不会留下任何
 * 用户态信息，服务日志停在被中断的那一行，事后只能推测。本机制在启动期
 * 注册一次性信号处理器，把信号号、出错地址与逐帧「模块(+偏移)」写入
 * stderr（守护的 stderr 已被启动方重定向到服务日志）。
 *
 * 偏移相对模块基址，与运行地址无关；配合同一构建产物即可离线 addr2line
 * 还原调用栈，无需 -rdynamic、无需 core dump 权限，Windows 侧为空实现。
 */

#include "daemon_crash.h"

#if (defined(__linux__) || defined(__APPLE__)) && defined(__has_include)
#if __has_include(<execinfo.h>)
#define CRASH_HAVE_EXECINFO 1
#endif
#endif

#ifdef CRASH_HAVE_EXECINFO

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#define CRASH_FRAMES 32

static void put_raw(const char *s, size_t n)
{
    ssize_t w;

    while (n > 0) {
        w = write(STDERR_FILENO, s, n);
        if (w <= 0)
            return;
        s += (size_t)w;
        n -= (size_t)w;
    }
}

static void put_str(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    put_raw(s, n);
}

static void put_num(int v)
{
    char buf[12];
    int i = (int)sizeof(buf);

    if (v <= 0) {
        put_raw("0", 1);
        return;
    }
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    }
    put_raw(buf + i, sizeof(buf) - (size_t)i);
}

static const char *sig_name(int sig)
{
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    default:
        return "SIGNAL";
    }
}

static void crash_trace(int sig, siginfo_t *si, void *uc)
{
    void *frames[CRASH_FRAMES];
    int n;

    (void)uc;
    n = backtrace(frames, CRASH_FRAMES);
    put_str("fatal: ");
    put_str(sig_name(sig));
    put_str(" code=");
    put_num(si ? si->si_code : 0);
    put_str(" frames=");
    put_num(n);
    put_str("\n");
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    put_str("fatal: end of trace\n");
}

void daemon_crash(void)
{
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    static int armed;
    struct sigaction sa;
    void *warm[2];

    if (armed)
        return;
    armed = 1;

    /* glibc 首次 backtrace 会 dlopen libgcc 并取锁；在信号上下文里做这事
     * 可能死锁。启动期先走一遍，把这条惰性路径提前走完。 */
    (void)backtrace(warm, 2);

    sa.sa_sigaction = crash_trace;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        (void)sigaction(sigs[i], &sa, NULL);
}

#else

void daemon_crash(void)
{
}

#endif
