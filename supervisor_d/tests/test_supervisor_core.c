// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_supervisor_core.c
 * @brief supervisor_d 核心单元测试：退避曲线 / JSON 提取 / 声明解析 /
 *        健康探测 / 控制口往返。
 *
 * 被测四源件（decl/proc/probe/ctrl）直接编入（不含 main.c）。全平台腿
 * 覆盖退避封顶、JSON 字段提取、内置缺省表与空转探测；POSIX 腿覆盖
 * profile.env 双遍解析、env 回落、UDS 探测、假死强杀收割链路与控制口
 * health_check / shutdown 往返。
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
#include <sys/wait.h>
#include <unistd.h>
#endif

#define TEST_OK() \
    do { printf("  %-46s PASS\n", __func__); return 0; } while (0)
#define TEST_FAIL(msg) \
    do { printf("  %-46s FAIL: %s\n", __func__, msg); return -1; } while (0)
#define TEST_ASSERT(cond, msg) \
    do { if (!(cond)) TEST_FAIL(msg); } while (0)

#ifndef _WIN32
static char g_dir[160];
static char g_ep[192];

static int mk_tmpdir(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/sup_test_%d", (int)getpid());
    if (mkdir(g_dir, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static void rm_tmpdir(void)
{
    char p[224];
    snprintf(p, sizeof(p), "%s/config/profile.env", g_dir);
    unlink(p);
    snprintf(p, sizeof(p), "%s/config", g_dir);
    rmdir(p);
    snprintf(p, sizeof(p), "%s/sup.sock", g_dir);
    unlink(p);
    snprintf(p, sizeof(p), "%s/probe.sock", g_dir);
    unlink(p);
    snprintf(p, sizeof(p), "%s/nope.sock", g_dir);
    unlink(p);
    rmdir(g_dir);
}

static void env_clear(void)
{
    unsetenv("AIRY_HOME");
    unsetenv("AIRY_RUNTIME_DIR");
    unsetenv("AIRY_SUPERVISOR_SOCK");
    unsetenv("AIRYRT_SUP_TICK_MS");
    unsetenv("AIRYRT_SUP_BACKOFF_BASE_MS");
    unsetenv("AIRYRT_SUP_BACKOFF_MAX_MS");
    unsetenv("AIRYRT_SUP_MAX_ATTEMPTS");
    unsetenv("AIRYRT_SUP_LIVENESS_TICKS");
}

/* 控制口客户端子进程：连接 g_ep，发单行请求，按断言词判定退出码 */
static void client_roundtrip(const char *req, const char *n1, const char *n2,
                             const char *n3)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        _exit(2);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", g_ep);
    if (connect(fd, (const struct sockaddr *)&sa, sizeof(sa)) != 0)
        _exit(3);
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (write(fd, req, strlen(req)) != (ssize_t)strlen(req))
        _exit(4);
    char buf[512];
    size_t tot = 0;
    for (;;) {
        ssize_t n = read(fd, buf + tot, sizeof(buf) - 1 - tot);
        if (n <= 0)
            break;
        tot += (size_t)n;
        if (memchr(buf, '\n', tot) || tot + 1 >= sizeof(buf))
            break;
    }
    buf[tot] = '\0';
    int ok = (!n1 || strstr(buf, n1)) && (!n2 || strstr(buf, n2)) &&
             (!n3 || strstr(buf, n3));
    _exit(ok ? 0 : 5);
}
#endif /* !_WIN32 */

/* ---- 全平台腿 ---- */

static int t_backoff(void)
{
    sup_ctx_t ctx;
    sup_proc_t p;
    memset(&ctx, 0, sizeof(ctx));
    memset(&p, 0, sizeof(p));
    ctx.backoff_base_ms = 1000;
    ctx.backoff_max_ms = 30000;
    const int fc[] = {1, 2, 3, 4, 5, 6, 20};
    const long long want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (size_t i = 0; i < sizeof(fc) / sizeof(fc[0]); i++) {
        p.fail_count = fc[i];
        if (sup_backoff_ms(&ctx, &p) != want[i])
            TEST_FAIL("default curve");
    }
    ctx.backoff_base_ms = 100;
    ctx.backoff_max_ms = 250;
    const int fc2[] = {1, 2, 3};
    const long long want2[] = {100, 200, 250};
    for (size_t i = 0; i < sizeof(fc2) / sizeof(fc2[0]); i++) {
        p.fail_count = fc2[i];
        if (sup_backoff_ms(&ctx, &p) != want2[i])
            TEST_FAIL("custom cap");
    }
    TEST_OK();
}

static int t_json_basic(void)
{
    char out[64];
    TEST_ASSERT(sup_json_field("{\"name\":\"gateway_d\"}", "name",
                               out, sizeof(out)) == 0 &&
                strcmp(out, "gateway_d") == 0, "plain field");
    TEST_ASSERT(sup_json_field("{\"name\" : \"gateway_d\"}", "name",
                               out, sizeof(out)) == 0 &&
                strcmp(out, "gateway_d") == 0, "colon spacing");
    TEST_ASSERT(sup_json_field("{\"msg\":\"a\\\"b\"}", "msg",
                               out, sizeof(out)) == 0 &&
                strcmp(out, "a\"b") == 0, "escape unquote");
    TEST_OK();
}

static int t_json_errs(void)
{
    char out[64];
    TEST_ASSERT(sup_json_field("{\"other\":\"x\"}", "name",
                               out, sizeof(out)) == -1, "missing key");
    TEST_ASSERT(sup_json_field("{\"name\":7}", "name",
                               out, sizeof(out)) == -1, "non-string");
    TEST_ASSERT(sup_json_field("{\"name\":\"abc", "name",
                               out, sizeof(out)) == -1, "unterminated");
    TEST_ASSERT(sup_json_field("{\"name\":\"gateway_d\"}", "name",
                               out, 4) == 0 && strcmp(out, "gat") == 0,
                "truncation");
    TEST_OK();
}

static int t_decl_defaults(void)
{
    sup_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT(sup_decl_defaults(&ctx) == 0, "defaults failed");
    TEST_ASSERT(ctx.count == 14, "count != 14");
    TEST_ASSERT(strcmp(ctx.procs[0].name, "gateway_d") == 0 &&
                ctx.procs[0].role == SUP_ROLE_CORE, "procs[0] core");
    TEST_ASSERT(strcmp(ctx.procs[13].name, "notify_d") == 0 &&
                ctx.procs[13].role == SUP_ROLE_AUX, "procs[13] aux");
    TEST_ASSERT(sup_proc_find(&ctx, "cupolas_d") == 10, "cupolas index");
    TEST_ASSERT(sup_proc_find(&ctx, "ghost_d") == -1, "ghost found");
    TEST_ASSERT(sup_proc_find(&ctx, "") == -1, "empty found");
    TEST_ASSERT(sup_proc_find(&ctx, NULL) == -1, "NULL found");
    TEST_OK();
}

static int t_health_noop(void)
{
    sup_ctx_t ctx;
    sup_proc_t p;
    memset(&ctx, 0, sizeof(ctx));
    memset(&p, 0, sizeof(p));
    snprintf(p.name, sizeof(p.name), "noop_d");
    p.pid = SUP_PID_INVALID;
    p.state = SUP_ST_BACKOFF;
    p.dead_ticks = 7;
    ctx.liveness_ticks = 3;
    sup_health_tick(&ctx, &p);
    TEST_ASSERT(p.state == SUP_ST_BACKOFF, "state changed");
    TEST_ASSERT(p.dead_ticks == 7, "dead_ticks changed");
    TEST_OK();
}

/* ---- POSIX 腿 ---- */

#ifndef _WIN32

static int t_decl_load(void)
{
    env_clear();
    if (mk_tmpdir() != 0)
        TEST_FAIL("mkdir tmp");
    char cfg[192], pf[224], sockp[224];
    snprintf(cfg, sizeof(cfg), "%s/config", g_dir);
    if (mkdir(cfg, 0755) != 0 && errno != EEXIST)
        TEST_FAIL("mkdir config");
    snprintf(pf, sizeof(pf), "%s/profile.env", cfg);
    FILE *f = fopen(pf, "w");
    if (!f)
        TEST_FAIL("open profile.env");
    fputs("AIRYRT_LAUNCH_CORE=gateway_d llm_d\n", f);
    fputs("AIRYRT_LAUNCH_AUX=cupolas_d\n", f);
    fputs("AIRYRT_LAUNCH_ARGS_gateway_d=--verbose --port 9000\n", f);
    fputs("AIRYRT_LAUNCH_ARGS_ghost_d=--ignored\n", f);
    fclose(f);

    snprintf(sockp, sizeof(sockp), "%s/sup.sock", g_dir);
    setenv("AIRY_HOME", g_dir, 1);
    setenv("AIRY_RUNTIME_DIR", g_dir, 1);
    setenv("AIRY_SUPERVISOR_SOCK", sockp, 1);
    setenv("AIRYRT_SUP_TICK_MS", "1234", 1);

    sup_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT(sup_decl_load(&ctx) == 0, "load failed");
    TEST_ASSERT(ctx.count == 3, "count != 3");
    int gi = sup_proc_find(&ctx, "gateway_d");
    TEST_ASSERT(gi == 0 && ctx.procs[gi].role == SUP_ROLE_CORE,
                "gateway_d core");
    TEST_ASSERT(strcmp(ctx.procs[gi].args, "--verbose --port 9000") == 0,
                "gateway_d args");
    int li = sup_proc_find(&ctx, "llm_d");
    TEST_ASSERT(li == 1 && ctx.procs[li].args[0] == '\0' &&
                ctx.procs[li].role == SUP_ROLE_CORE, "llm_d core/noargs");
    int ci = sup_proc_find(&ctx, "cupolas_d");
    TEST_ASSERT(ci == 2 && ctx.procs[ci].role == SUP_ROLE_AUX,
                "cupolas_d aux");
    TEST_ASSERT(sup_proc_find(&ctx, "ghost_d") == -1, "ghost registered");
    TEST_ASSERT(ctx.tick_ms == 1234, "tick env");
    TEST_ASSERT(strcmp(ctx.ctrl_ep, sockp) == 0, "ctrl_ep env");
    TEST_ASSERT(strcmp(ctx.runtime_dir, g_dir) == 0, "runtime_dir env");

    env_clear();
    rm_tmpdir();
    TEST_OK();
}

static int t_decl_fallback(void)
{
    env_clear();
    if (mk_tmpdir() != 0)
        TEST_FAIL("mkdir tmp");
    char cfg[192], pf[224];
    snprintf(cfg, sizeof(cfg), "%s/config", g_dir);
    mkdir(cfg, 0755);
    snprintf(pf, sizeof(pf), "%s/profile.env", cfg);
    FILE *f = fopen(pf, "w");
    if (!f)
        TEST_FAIL("open profile.env");
    fputs("AIRYRT_LAUNCH_ARGS_gateway_d=--orphan\n", f); /* 无 launch 段 */
    fclose(f);
    setenv("AIRY_HOME", g_dir, 1);

    sup_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT(sup_decl_load(&ctx) == 0, "load failed");
    TEST_ASSERT(ctx.count == 14, "no-launch must fall back to 14");
    TEST_ASSERT(ctx.tick_ms == SUP_DEFAULT_TICK_MS, "tick default");
    TEST_ASSERT(ctx.backoff_base_ms == SUP_DEFAULT_BACKOFF_BASE_MS,
                "base default");
    TEST_ASSERT(ctx.backoff_max_ms == SUP_DEFAULT_BACKOFF_MAX_MS,
                "max default");
    TEST_ASSERT(ctx.max_attempts == SUP_DEFAULT_MAX_ATTEMPTS,
                "attempts default");
    TEST_ASSERT(ctx.liveness_ticks == SUP_DEFAULT_LIVENESS_TICKS,
                "liveness default");

    unlink(pf); /* 腿2：文件缺失 + 非法 env → 全回落 */
    setenv("AIRYRT_SUP_TICK_MS", "abc", 1);
    setenv("AIRYRT_SUP_MAX_ATTEMPTS", "-5", 1);
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT(sup_decl_load(&ctx) == 0, "load nofile failed");
    TEST_ASSERT(ctx.count == 14, "missing file must fall back");
    TEST_ASSERT(ctx.tick_ms == SUP_DEFAULT_TICK_MS, "bad tick falls back");
    TEST_ASSERT(ctx.max_attempts == SUP_DEFAULT_MAX_ATTEMPTS,
                "bad attempts falls back");
    TEST_ASSERT(strstr(ctx.ctrl_ep, "run/supervisor.sock") != NULL,
                "default ctrl_ep");

    env_clear();
    rm_tmpdir();
    TEST_OK();
}

static int t_health_sock_up(void)
{
    sup_ctx_t ctx;
    sup_proc_t p;
    memset(&ctx, 0, sizeof(ctx));
    memset(&p, 0, sizeof(p));
    snprintf(p.name, sizeof(p.name), "tool_d");
    p.pid = getpid(); /* 自身进程：真实存活 */
    p.state = SUP_ST_STARTING;
    sup_health_tick(&ctx, &p); /* sock 为空 → 仅进程存活腿 */
    TEST_ASSERT(p.state == SUP_ST_RUNNING, "not promoted");
    TEST_ASSERT(p.fail_count == 0, "fail_count set");
    TEST_OK();
}

static int t_health_probe_ok(void)
{
    if (mk_tmpdir() != 0)
        TEST_FAIL("mkdir tmp");
    sup_ctx_t ctx;
    sup_proc_t p;
    memset(&ctx, 0, sizeof(ctx));
    memset(&p, 0, sizeof(p));
    snprintf(ctx.ctrl_ep, sizeof(ctx.ctrl_ep), "%s/probe.sock", g_dir);
    int lfd = sup_ctrl_listen(&ctx);
    TEST_ASSERT(lfd >= 0, "listen");
    snprintf(p.name, sizeof(p.name), "gateway_d");
    snprintf(p.sock, sizeof(p.sock), "%s", ctx.ctrl_ep);
    p.pid = getpid();
    p.state = SUP_ST_STARTING;
    p.dead_ticks = 2;
    ctx.liveness_ticks = 2;
    sup_health_tick(&ctx, &p);
    TEST_ASSERT(p.state == SUP_ST_RUNNING, "probe not ok");
    TEST_ASSERT(p.dead_ticks == 0, "dead_ticks not reset");
    TEST_ASSERT(p.fail_count == 0, "fail_count not reset");
    sup_ctrl_close(lfd);
    unlink(ctx.ctrl_ep);
    rm_tmpdir();
    TEST_OK();
}

static int t_health_kill(void)
{
    if (mk_tmpdir() != 0)
        TEST_FAIL("mkdir tmp");
    pid_t child = fork();
    if (child == 0) {
        for (;;)
            pause();
    }
    TEST_ASSERT(child > 0, "fork");
    sup_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.count = 1;
    ctx.liveness_ticks = 2;
    ctx.backoff_base_ms = 1000;
    ctx.backoff_max_ms = 30000;
    ctx.max_attempts = 5;
    sup_proc_t *p = &ctx.procs[0];
    snprintf(p->name, sizeof(p->name), "llm_d");
    snprintf(p->sock, sizeof(p->sock), "%s/nope.sock", g_dir);
    p->pid = child;
    p->state = SUP_ST_RUNNING;
    p->role = SUP_ROLE_CORE;

    sup_health_tick(&ctx, p);
    TEST_ASSERT(p->dead_ticks == 1, "dead_ticks after tick1");
    TEST_ASSERT(p->pid == child, "pid changed too early");

    sup_health_tick(&ctx, p);
    TEST_ASSERT(p->dead_ticks == 2, "dead_ticks after tick2");

    int reaped = 0;
    for (int i = 0; i < 300; i++) {
        sup_proc_reap(&ctx);
        if (p->state == SUP_ST_BACKOFF) {
            reaped = 1;
            break;
        }
        usleep(10000);
    }
    TEST_ASSERT(reaped, "reap timeout");
    TEST_ASSERT(p->fail_count == 1, "fail_count");
    TEST_ASSERT(strstr(p->last_death, "signal=9") != NULL, "last_death");
    TEST_ASSERT(p->next_ms > 0, "next_ms");
    rm_tmpdir();
    TEST_OK();
}

static int t_ctrl_stale(void)
{
    if (mk_tmpdir() != 0)
        TEST_FAIL("mkdir tmp");
    sup_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.ctrl_ep, sizeof(ctx.ctrl_ep), "%s/sup.sock", g_dir);
    unlink(ctx.ctrl_ep);
    int fd1 = sup_ctrl_listen(&ctx);
    TEST_ASSERT(fd1 >= 0, "listen 1");
    close(fd1); /* 故意留 stale socket 文件 */
    int fd2 = sup_ctrl_listen(&ctx);
    TEST_ASSERT(fd2 >= 0, "stale rebind");
    struct stat st;
    TEST_ASSERT(stat(ctx.ctrl_ep, &st) == 0, "stat sock");
    TEST_ASSERT((st.st_mode & 0777) == 0600, "perm 0600");
    sup_ctrl_close(fd2);
    unlink(ctx.ctrl_ep);
    rm_tmpdir();
    TEST_OK();
}

static int t_ctrl_serve(void)
{
    if (mk_tmpdir() != 0)
        TEST_FAIL("mkdir tmp");
    sup_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(g_ep, sizeof(g_ep), "%s/sup.sock", g_dir);
    snprintf(ctx.ctrl_ep, sizeof(ctx.ctrl_ep), "%s", g_ep);
    ctx.count = 2;
    snprintf(ctx.procs[0].name, sizeof(ctx.procs[0].name), "gateway_d");
    ctx.procs[0].state = SUP_ST_RUNNING;
    ctx.procs[1].state = SUP_ST_BACKOFF;
    ctx.procs[1].fail_count = 2;
    snprintf(ctx.procs[1].name, sizeof(ctx.procs[1].name), "cupolas_d");

    /* 腿1：health_check 往返 */
    int lfd = sup_ctrl_listen(&ctx);
    TEST_ASSERT(lfd >= 0, "listen");
    pid_t c = fork();
    if (c == 0)
        client_roundtrip("{\"method\":\"health_check\",\"id\":5}\n",
                         "\"daemons\"", "gateway_d", "\"id\":5");
    TEST_ASSERT(c > 0, "fork");
    sup_ctrl_serve(&ctx, lfd, 3000);
    int st = 0;
    waitpid(c, &st, 0);
    sup_ctrl_close(lfd);
    TEST_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "health_check leg");
    TEST_ASSERT(ctx.shutdown == 0, "shutdown leaked");

    /* 腿2：supervisor.shutdown 受理 */
    ctx.shutdown = 0;
    lfd = sup_ctrl_listen(&ctx);
    TEST_ASSERT(lfd >= 0, "listen 2");
    c = fork();
    if (c == 0)
        client_roundtrip("{\"method\":\"supervisor.shutdown\",\"id\":7}\n",
                         "\"result\":\"ok\"", "\"id\":7", NULL);
    TEST_ASSERT(c > 0, "fork 2");
    sup_ctrl_serve(&ctx, lfd, 3000);
    waitpid(c, &st, 0);
    sup_ctrl_close(lfd);
    unlink(g_ep);
    rm_tmpdir();
    TEST_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "shutdown leg");
    TEST_ASSERT(ctx.shutdown == 1, "shutdown not set");
    TEST_OK();
}

#endif /* !_WIN32 */

int main(void)
{
    int run = 0, pass = 0;
    run++;
    pass += t_backoff() == 0;
    run++;
    pass += t_json_basic() == 0;
    run++;
    pass += t_json_errs() == 0;
    run++;
    pass += t_decl_defaults() == 0;
    run++;
    pass += t_health_noop() == 0;
#ifndef _WIN32
    run++;
    pass += t_decl_load() == 0;
    run++;
    pass += t_decl_fallback() == 0;
    run++;
    pass += t_health_sock_up() == 0;
    run++;
    pass += t_health_probe_ok() == 0;
    run++;
    pass += t_health_kill() == 0;
    run++;
    pass += t_ctrl_stale() == 0;
    run++;
    pass += t_ctrl_serve() == 0;
#endif
    printf("supervisor_core: %d/%d passed\n", pass, run);
    return pass == run ? 0 : 1;
}
