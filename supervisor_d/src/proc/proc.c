// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file proc.c
 * @brief 进程域：spawn / reap / 指数退避 / 调谐 / 假死探测 / 按需激活 / 收摊。
 *
 * spawn：POSIX fork/exec + setsid + PR_SET_PDEATHSIG（防孤儿）+ 输出经
 *        pipe 交给 pump 线程惰性落盘到 $AIRY_HOME/logs/<name>.out（首个
 *        字节才建文件，A-5；AIRY_PREOPEN_OUT=1 回退预开）；Windows
 *        (CREATE_SUSPENDED) + Job Object KILL_ON_JOB_CLOSE。
 *        POSIX 子进程分支先做 LD_* 清洗（隔离格）：宿主注入的
 *        LD_LIBRARY_PATH / LD_PRELOAD / LD_AUDIT 一律不继承，仅当
 *        画像声明 AIRYRT_LAUNCH_KEEPENV_<NAME> 显式列出时保留。
 * reap：waitpid(WNOHANG) / WaitForSingleObject(0) 收割死因写入
 *       last_death（V13.1 判据）；CORE 死进 BACKOFF，AUX 死回 STOPPED
 *       （仅 activate 复活）。A-9：BACKOFF/WAIT 到期由 sup_next_wake_ms
 *       提前唤醒主循环；AIRY_SUPERVISOR_NO_RESPAWN=1 时转 FAILED 不重建。
 * 调谐：sup_reconcile 每周期比对期望态与实际态；连败超限转 FAILED 并
 *       显式告警，不再自动重启（V13.3 防风暴）；复位 FAILED 的唯一
 *       入口是 sup_activate 显式激活。
 * 探测：双层判定之第二层（sup_health_tick）——进程存活由 reap 层判定，
 *       本层对已存活进程做 sock 可达性探测（机制委托 net.c 的
 *       sup_probe_sock，非阻塞 connect，500ms 超时），STARTING→RUNNING
 *       迁移以此佐证；连续 liveness_ticks 个周期不可达判定假死并强杀，
 *       交由 reap 记录死因进入退避。sock 为空者（Windows 无登记端点的
 *       maths）仅进程存活判定。
 */

#include "supervisor_d_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

/* 单调毫秒：全户计时唯一钟（墙钟只用于日志时间戳） */
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
/* ==================== 输出泵（A-5，POSIX only） ====================
 *
 * 旧实现由子进程在 exec 前以 O_CREAT 预开 <name>.out，无任何输出的 daemon
 * 也会留下 0 字节文件。改为：父进程建 pipe，子进程把写端 dup2 到
 * STDOUT/STDERR；单一 pump 线程 poll 汇聚全部读端，首个字节到达时才
 * fopen(...,"a") 建文件并持续落盘，EOF/错误即关档回收槽位。
 * AIRY_PREOPEN_OUT=1 回退旧预开行为（排障与兼容）；Windows 腿保持预开。
 *
 * 并发模型：pump 线程与主线程（add/stop）共享槽位，全部经 pm->lock 串行；
 * 事件循环先在锁内抄一份 fd 集合再 poll，醒后按槽位下标而非 poll 返回
 * 的 fd 号读取当前槽位，故槽位被替换不会误读无关 fd。读端一律
 * O_NONBLOCK，锁内读空至 EAGAIN，单轮上限 SUP_PUMP_BURST 字节防止
 * 高产 daemon 长期占锁。
 */

#define SUP_PUMP_BUF 4096
#define SUP_PUMP_BURST (256 * 1024)

int sup_preopen_env(void)
{
    const char *v = getenv("AIRY_PREOPEN_OUT");
    return v && v[0] == '1' && v[1] == '\0';
}

static void pump_close_slot(sup_pump_t *pm, int slot)
{
    if (pm->fp[slot]) {
        fclose(pm->fp[slot]);
        pm->fp[slot] = NULL;
    }
    if (pm->rfd[slot] >= 0) {
        close(pm->rfd[slot]);
        pm->rfd[slot] = -1;
    }
}

/* 锁内：把一个槽位读到 EAGAIN/EOF/错误。返回本次落盘字节数。 */
static long pump_drain(sup_pump_t *pm, int slot)
{
    char buf[SUP_PUMP_BUF];
    long total = 0;
    while (pm->rfd[slot] >= 0 && total < SUP_PUMP_BURST) {
        ssize_t n = read(pm->rfd[slot], buf, sizeof(buf));
        if (n > 0) {
            if (!pm->fp[slot] && pm->out[slot][0] != '\0')
                pm->fp[slot] = fopen(pm->out[slot], "a");
            if (pm->fp[slot]) {
                size_t w = fwrite(buf, 1, (size_t)n, pm->fp[slot]);
                if (w != (size_t)n) { /* 落盘失败：关档丢本块，下轮重试建文件 */
                    fclose(pm->fp[slot]);
                    pm->fp[slot] = NULL;
                } else if (fflush(pm->fp[slot]) != 0) {
                    fclose(pm->fp[slot]);
                    pm->fp[slot] = NULL;
                }
            }
            total += (long)n;
            continue;
        }
        if (n == 0) { /* EOF：子进程及其后代均已关闭写端 */
            pump_close_slot(pm, slot);
            break;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        pump_close_slot(pm, slot);
        break;
    }
    return total;
}

static void pump_wakeup(const sup_pump_t *pm)
{
    if (pm->wake[1] >= 0) {
        char c = 1;
        ssize_t ignore = write(pm->wake[1], &c, 1);
        (void)ignore;
    }
}

static void pump_clear_wake(sup_pump_t *pm)
{
    char buf[64];
    while (pm->wake[0] >= 0 && read(pm->wake[0], buf, sizeof(buf)) > 0)
        ;
}

static void *pump_main(void *arg)
{
    sup_ctx_t *ctx = (sup_ctx_t *)arg;
    sup_pump_t *pm = &ctx->pump;
    struct pollfd pfd[SUP_MAX_DAEMONS + 1];
    int slot_of[SUP_MAX_DAEMONS + 1];

    for (;;) {
        int n = 0, stop = 0;
        pthread_mutex_lock(&pm->lock);
        stop = pm->stop;
        pfd[0].fd = pm->wake[0];
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        slot_of[0] = -1;
        n = 1;
        for (int i = 0; i < SUP_MAX_DAEMONS; i++) {
            if (pm->rfd[i] < 0)
                continue;
            pfd[n].fd = pm->rfd[i];
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            slot_of[n] = i;
            n++;
        }
        if (stop) { /* 收摊：先读空全部槽位再退，避免丢尾部日志 */
            for (int i = 0; i < SUP_MAX_DAEMONS; i++)
                while (pm->rfd[i] >= 0 && pump_drain(pm, i) >= SUP_PUMP_BURST)
                    ;
            pthread_mutex_unlock(&pm->lock);
            break;
        }
        pthread_mutex_unlock(&pm->lock);

        int rc = poll(pfd, (nfds_t)n, 200);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0)
            continue;

        pthread_mutex_lock(&pm->lock);
        if (pfd[0].revents & POLLIN)
            pump_clear_wake(pm);
        for (int k = 1; k < n; k++) {
            if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
                continue;
            int slot = slot_of[k];
            if (slot < 0 || slot >= SUP_MAX_DAEMONS || pm->rfd[slot] < 0)
                continue;
            pump_drain(pm, slot);
        }
        pthread_mutex_unlock(&pm->lock);
    }
    return NULL;
}

int sup_pump_start(sup_ctx_t *ctx)
{
    sup_pump_t *pm = &ctx->pump;

    if (pm->running)
        return 0;
    for (int i = 0; i < SUP_MAX_DAEMONS; i++) {
        pm->rfd[i] = -1;
        pm->fp[i] = NULL;
        pm->out[i][0] = '\0';
    }
    pm->wake[0] = pm->wake[1] = -1;
    pm->stop = 0;
    if (pipe(pm->wake) != 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        fcntl(pm->wake[i], F_SETFL, fcntl(pm->wake[i], F_GETFL, 0) | O_NONBLOCK);
        fcntl(pm->wake[i], F_SETFD, FD_CLOEXEC);
    }
    if (pthread_mutex_init(&pm->lock, NULL) != 0) {
        close(pm->wake[0]);
        close(pm->wake[1]);
        pm->wake[0] = pm->wake[1] = -1;
        return -1;
    }
    pm->running = 1; /* 先置位：线程首轮读 stop 需要看到已就绪 */
    if (pthread_create(&pm->tid, NULL, pump_main, ctx) != 0) {
        pm->running = 0;
        pthread_mutex_destroy(&pm->lock);
        close(pm->wake[0]);
        close(pm->wake[1]);
        pm->wake[0] = pm->wake[1] = -1;
        return -1;
    }
    return 0;
}

void sup_pump_stop(sup_ctx_t *ctx)
{
    sup_pump_t *pm = &ctx->pump;

    if (!pm->running)
        return;
    pthread_mutex_lock(&pm->lock);
    pm->stop = 1;
    pthread_mutex_unlock(&pm->lock);
    pump_wakeup(pm);
    pthread_join(pm->tid, NULL);
    for (int i = 0; i < SUP_MAX_DAEMONS; i++)
        pump_close_slot(pm, i);
    close(pm->wake[0]);
    close(pm->wake[1]);
    pm->wake[0] = pm->wake[1] = -1;
    pthread_mutex_destroy(&pm->lock);
    pm->stop = 0;
    pm->running = 0;
}

int sup_pump_add(sup_ctx_t *ctx, int slot, int rfd, const char *out)
{
    sup_pump_t *pm = &ctx->pump;

    if (!pm->running || slot < 0 || slot >= SUP_MAX_DAEMONS || rfd < 0)
        return -1;
    fcntl(rfd, F_SETFL, fcntl(rfd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(rfd, F_SETFD, FD_CLOEXEC);
    pthread_mutex_lock(&pm->lock);
    /* 同槽新一代接管：先读空上一代残留（其 pid 已收割，写端通常已关） */
    while (pm->rfd[slot] >= 0 && pump_drain(pm, slot) >= SUP_PUMP_BURST)
        ;
    pump_close_slot(pm, slot);
    pm->rfd[slot] = rfd;
    snprintf(pm->out[slot], SUP_PATH_MAX, "%s", out ? out : "");
    pthread_mutex_unlock(&pm->lock);
    pump_wakeup(pm);
    return 0;
}

static int env_keep(const char *list, const char *var)
{
    size_t n = strlen(var);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t')
            e++;
        if ((size_t)(e - p) == n && strncmp(p, var, n) == 0)
            return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

void sup_env_scrub(const sup_proc_t *p)
{
    static const char *const dyn[] = {"LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT"};
    for (size_t i = 0; i < sizeof(dyn) / sizeof(dyn[0]); i++) {
        if (!getenv(dyn[i]))
            continue;
        if (p && env_keep(p->keepenv, dyn[i]))
            continue;
        unsetenv(dyn[i]);
    }
}

static int spawn_posix(sup_ctx_t *ctx, sup_proc_t *p, int slot)
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

    char out[SUP_PATH_MAX];
    snprintf(out, sizeof(out), "%s/%s.out", ctx->log_dir, p->name);

    /* A-5：优先经输出泵（惰性建文件）；泵未起或显式回退时走旧预开路径 */
    int pfd[2] = {-1, -1};
    int routed = (!sup_preopen_env() && ctx->pump.running) ? 1 : 0;
    if (routed && pipe(pfd) != 0) {
        sup_log("WARN", "%s pipe failed: %s, preopen fallback", p->name,
                strerror(errno));
        routed = 0;
    }

#ifdef __linux__
    /* PDEATHSIG race 检查仅 Linux 可用，ppid 须在 fork 前采样 */
    pid_t ppid = getpid();
#endif
    pid_t pid = fork();
    if (pid < 0) {
        if (routed) {
            close(pfd[0]);
            close(pfd[1]);
        }
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
        if (routed) {
            dup2(pfd[1], STDOUT_FILENO);
            dup2(pfd[1], STDERR_FILENO);
            if (pfd[0] > STDERR_FILENO)
                close(pfd[0]);
            if (pfd[1] > STDERR_FILENO)
                close(pfd[1]);
        } else {
            int fd = open(out, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > STDERR_FILENO)
                    close(fd);
            }
        }
        sup_env_scrub(p);
        execv(p->bin, argv);
        _exit(127);
    }
    if (routed) {
        close(pfd[1]); /* 写端仅子进程持有，父端必须关，否则永无 EOF */
        if (sup_pump_add(ctx, slot, pfd[0], out) != 0) {
            close(pfd[0]);
            sup_log("WARN", "%s pump attach failed, output dropped", p->name);
        }
    }
    p->pid = pid;
    sup_log("INFO", "spawn %s pid=%d attempt=%d", p->name, (int)p->pid,
            p->fail_count + 1);
    return 0;
}
#endif

static int sup_proc_spawn(sup_ctx_t *ctx, sup_proc_t *p)
{
    int rc;

    if (p->pid != SUP_PID_INVALID)
        return SUP_SPAWN_OK;
    /* 装不下就不派生：返回 DEFER，由调用方按 WAIT 处理，不计连败 */
    if (sup_admit_room(p, NULL) == 0)
        return SUP_SPAWN_DEFER;
#ifdef _WIN32
    rc = spawn_win(ctx, p);
#else
    /* 泵槽位按 procs 下标固定（p 恒指向 ctx->procs 数组内元素） */
    rc = spawn_posix(ctx, p, (int)(p - ctx->procs));
#endif
    if (rc != 0) {
        p->fail_count++;
        return SUP_SPAWN_FAIL;
    }
    p->state = SUP_ST_STARTING;
    p->dead_ticks = 0;
    p->ready_until = sup_now_ms() + (p->ready_ms > 0 ? p->ready_ms : ctx->startup_grace_ms);
    return SUP_SPAWN_OK;
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
            if (ctx->no_respawn) {
                /* A-9 回退开关：只留死因，不自动重建；显式 activate 仍可复活 */
                p->state = SUP_ST_FAILED;
                sup_log("WARN", "%s respawn disabled by AIRY_SUPERVISOR_NO_RESPAWN",
                        p->name);
            } else {
                p->state = SUP_ST_BACKOFF;
                p->next_ms = sup_now_ms() + sup_backoff_ms(ctx, p);
            }
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
    int rc = sup_proc_spawn(ctx, p);

    if (rc == SUP_SPAWN_OK)
        return;
    if (rc == SUP_SPAWN_DEFER) {
        /* WAIT 不是故障：内存回来之前只重判、不重派生，连败计数不增长 */
        p->state = SUP_ST_WAIT;
        p->next_ms = now + ctx->admit_retry_ms;
        return;
    }
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
                            "%s FAILED after %d attempts, manual activate; out: %s/%s.out",
                            p->name, p->fail_count, ctx->log_dir, p->name);
                } else {
                    sup_log("INFO", "%s respawn after %lldms backoff (fail=%d)",
                            p->name, p->next_ms - now, p->fail_count);
                    spawn_or_backoff(ctx, p, now);
                }
            }
            break;
        case SUP_ST_WAIT:
            if (now >= p->next_ms)
                spawn_or_backoff(ctx, p, now);
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

/* A-9：下一个待办时刻——取所有 CORE 在 BACKOFF/WAIT 态的到期时间最小值，
 * 上界为常规 tick。主循环据此缩短 poll 超时，使 TERM 单守护后的重建
 * 不再被整拍对齐拖到 2×tick。 */
long long sup_next_wake_ms(const sup_ctx_t *ctx)
{
    long long now = sup_now_ms();
    long long wake = ctx->tick_ms;
    for (int i = 0; i < ctx->count; i++) {
        const sup_proc_t *p = &ctx->procs[i];
        if (p->role != SUP_ROLE_CORE)
            continue;
        if (p->state != SUP_ST_BACKOFF && p->state != SUP_ST_WAIT)
            continue;
        long long d = p->next_ms - now;
        if (d < 0)
            d = 0;
        if (d < wake)
            wake = d;
    }
    return wake < 1 ? 1 : wake;
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
    if (sup_probe_sock(p->sock) == 0) {
        if (p->state != SUP_ST_RUNNING)
            sup_log("INFO", "%s healthy (sock=%s)", p->name, p->sock);
        p->state = SUP_ST_RUNNING;
        p->dead_ticks = 0;
        p->fail_count = 0;
        return;
    }
    /* 启动宽限：自托管模型（llama.cpp）要先把权重读入内存才监听端口，
     * 期间探活必然失败。宽限窗口内不累计假死，否则 15s 就被 SIGKILL，
     * 大模型永远起不来。窗口过后恢复正常假死判定。 */
    if (p->ready_until > 0 && sup_now_ms() < p->ready_until) {
        if (p->state != SUP_ST_STARTING)
            p->state = SUP_ST_STARTING;
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
