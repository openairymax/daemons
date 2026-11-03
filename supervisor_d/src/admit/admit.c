// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file admit.c
 * @brief 准入域：派生受管进程前，先问操作系统「这台机器此刻装不装得下」。
 *
 * 判据不在本文件。受管进程在画像里声明 AIRYRT_LAUNCH_MEM_<NAME>（常驻
 * 字节）后，本域向 airy-rt-cap admit 取一次判定——保留量、MemAvailable
 * 口径、fit/refuse/unknown 三态全部由 AirymaxOS 的同一份实现给出，此处
 * 只消费结论。把口径复制到 C 里会得到两套随时会分叉的内存判据，现场
 * 也就无法解释「单元说装不下、启动器说装得下」。
 *
 * 判定只决定此刻派不派生。装不下时进程停在 WAIT 等内存回来：重试的是
 * 一次几毫秒的判定，不是一次几百兆的派生，因此不计连败、不进指数退避
 * ——与单元侧把 75/78 移出重启策略同一条原则（拒绝是结论，不是故障）。
 *
 * 工具缺失（非 AirymaxOS 主机、裁剪镜像、执行失败、超时）时不冒充可装
 * 也不冒充装不下：记一次告警后照常派生，等同 airy-rt-cap 的 unknown。
 *
 * POSIX：fork/exec airy-rt-cap，读 stdout 并解析 JSON；子进程整体限时
 *        2 秒，超时强杀，避免判定卡住调谐周期。
 * Windows：本机没有 airy-rt-cap，也没有对应的内存准入策略，一律报告
 *        「无策略」（不过滤），属平台事实而非桩实现。
 */

#include "supervisor_d_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

long sup_admit_room(const sup_proc_t *p, long long *short_out)
{
    (void)p;
    if (short_out)
        *short_out = 0;
    return -1;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define ADMIT_BIN "/usr/bin/airy-rt-cap"
#define ADMIT_LIMIT_MS 2000
#define ADMIT_OUT_MAX 512

/* 限时读干 pipe：返回字节数；超时或出错返回 -1（调用方按不可用处理） */
static int read_deadline(int fd, char *out, size_t sz)
{
    long long end = sup_now_ms() + ADMIT_LIMIT_MS;
    size_t n = 0;

    while (n + 1 < sz) {
        long long left = end - sup_now_ms();
        struct pollfd pfd;
        ssize_t got;

        if (left <= 0)
            return -1;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, (int)left) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got = read(fd, out + n, sz - 1 - n);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (got == 0)
            break;
        n += (size_t)got;
    }
    out[n] = '\0';
    return (int)n;
}

/* 跑一次 admit 取回 stdout。返回子进程退出码；执行路径本身失败返回 -1 */
static int admit_run(long long need, char *out, size_t sz)
{
    const char *bin = getenv("AIRYRT_ADMIT_CMD");
    char arg[32];
    int fds[2];
    pid_t pid;
    int got;
    int st = 0;

    if (!bin || !*bin)
        bin = ADMIT_BIN;
    snprintf(arg, sizeof(arg), "%lld", need);
    if (pipe(fds) != 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        int null_fd;
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        execl(bin, bin, "admit", "--need", arg, "--json", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    got = read_deadline(fds[0], out, sz);
    close(fds[0]);
    if (got < 0)
        kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    if (got < 0 || !WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

long sup_admit_room(const sup_proc_t *p, long long *short_out)
{
    char out[ADMIT_OUT_MAX];
    char state[16];
    long long short_by = 0;
    int rc;

    if (!p || p->need <= 0)
        return 1;
    rc = admit_run(p->need, out, sizeof(out));
    if (rc < 0 || rc == 127) {
        sup_log("WARN", "%s admit: no policy, spawn unfiltered", p->name);
        return -1;
    }
    if (sup_json_field(out, "state", state, sizeof(state)) != 0) {
        sup_log("WARN", "%s admit: unreadable verdict, spawn unfiltered",
                p->name);
        return -1;
    }
    if (strcmp(state, "refuse") == 0) {
        short_by = sup_json_num(out, "short", 0);
        if (short_out)
            *short_out = short_by;
        sup_log("WARN", "%s admit: refuse need=%lld short=%lld", p->name,
                p->need, short_by);
        return 0;
    }
    return 1; /* fit 与 unknown 都放行：unknown 不是「装不下」 */
}

#endif
