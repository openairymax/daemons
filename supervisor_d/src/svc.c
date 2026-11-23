// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc.c
 * @brief 常驻生命周期：prepare（就绪）→ step（单轮调谐+受理）→ clear（收摊），
 *        并含端到端就绪判定与边沿记账。
 *
 * V13.4/V13.5：pid 文件防重复启动（自持，无二级监管者）；主循环
 * reconcile→serve 每 tick 一轮；SIGTERM/SIGINT 与 supervisor.shutdown
 * 同路径收摊；退出清 pid 文件与 UDS 残留。零项目内库链接（V13.5）。
 */

#include "supervisor_d_internal.h"

#include <signal.h> /* sig_atomic_t（标准头，Windows 亦提供） */

#ifndef _WIN32
#include <unistd.h>
#endif

static volatile sig_atomic_t g_stop = 0;

#ifndef _WIN32
static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
}
#endif

int sup_svc_prepare(sup_ctx_t *ctx, int *lfd_out)
{
#ifdef _WIN32
    if (sup_proc_job_init() != 0)
        sup_log("WARN", "job object init failed, orphan guard disabled");
#endif
    ensure_dirs(ctx);
    if (sup_pidfile_write(ctx) != 0) {
        sup_log("ERROR", "another supervisor running (pid file)");
        return -1;
    }
#ifndef _WIN32
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL); /* reap 层 waitpid 依赖默认处置 */
#endif

    int ncore = 0, naux = 0;
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->procs[i].role == SUP_ROLE_CORE)
            ncore++;
        else
            naux++;
    }
    int lfd = sup_ctrl_listen(ctx);
    if (lfd < 0) {
        sup_log("ERROR", "ctrl listen failed: %s", ctx->ctrl_ep);
        sup_pidfile_clear(ctx);
        return -1;
    }
#ifndef _WIN32
    /* A-5：输出泵先起再放行派生；失败则 spawn 走旧预开 .out 路径 */
    if (sup_pump_start(ctx) != 0)
        sup_log("WARN", "output pump unavailable, preopen .out fallback");
#endif
    /* 这一行只声明期望态已经装载，不代表依赖可用；端到端就绪由
     * sup_ready_tick 在全 CORE 端点可达时单独记账（S-P2-03）。 */
    ctx->ready_deadline = sup_now_ms() + ctx->startup_grace_ms;
    sup_log("INFO",
            "up: home=%s declared %d daemons (core=%d aux=%d) tick=%ldms ep=%s",
            ctx->airy_home, ctx->count, ncore, naux, ctx->tick_ms,
            ctx->ctrl_ep);
    *lfd_out = lfd;
    return 0;
}

int sup_svc_step(sup_ctx_t *ctx, int lfd)
{
    if (g_stop || ctx->shutdown)
        return 1;
    sup_reconcile(ctx);
    if (g_stop || ctx->shutdown)
        return 1;
    sup_ready_tick(ctx); /* 就绪边沿记账：依赖可用与否以端点可达为准 */
    /* A-9：退避到期不必等整拍——按最近待办时刻收窄 poll 超时 */
    long long wake = sup_next_wake_ms(ctx);
    if (wake > ctx->tick_ms)
        wake = ctx->tick_ms;
    sup_ctrl_serve(ctx, lfd, (int)wake);
    return 0;
}

void sup_svc_clear(sup_ctx_t *ctx, int lfd)
{
    sup_shutdown_all(ctx);
#ifndef _WIN32
    sup_pump_stop(ctx); /* 读空并落盘尾部输出后再收控制口 */
#endif
    sup_ctrl_close(lfd);
#ifndef _WIN32
    unlink(ctx->ctrl_ep);
#endif
    sup_pidfile_clear(ctx);
}

/* ---- 端到端就绪（S-P2-03）---------------------------------------------
 * 历史缺陷：启动日志以「声明了几个 daemon」当成功，而此刻依赖往往尚未
 * 监听——运维看到 up，拿到的却是 PEP 连不上 notify_d、推理链探活 http=0
 * 的半可用系统。
 *
 * 判据与 reconcile 同源，不引入第二套健康口径：进程存活由 reap 层
 * （waitpid/WaitForSingleObject）证明，端点可达由 sup_health_tick 的 sock
 * 探测证明，二者齐备时状态机才置 RUNNING。因此「全部 CORE 处于 RUNNING」
 * 即「声明的依赖都已在服务」。AUX 是按需集，不计入启动判据。
 *
 * 状态迁移只在边沿发声：进入就绪记一条 INFO，丢失就绪记一次逐项缺口
 * （启动宽限内不报，避免把正常的派生窗口误报成故障），此后不刷屏。
 */

const char *sup_state_str(sup_state_t st)
{
    switch (st) {
    case SUP_ST_STARTING:
        return "starting";
    case SUP_ST_RUNNING:
        return "running";
    case SUP_ST_BACKOFF:
        return "backoff";
    case SUP_ST_FAILED:
        return "failed";
    case SUP_ST_WAIT:
        return "wait";
    default:
        return "stopped";
    }
}

sup_ready_t sup_ready_core(const sup_ctx_t *ctx)
{
    sup_ready_t r = {0, 0};
    for (int i = 0; i < ctx->count; i++) {
        const sup_proc_t *p = &ctx->procs[i];
        if (p->role != SUP_ROLE_CORE)
            continue;
        r.core_total++;
        if (p->state == SUP_ST_RUNNING)
            r.core_ready++;
    }
    return r;
}

void sup_ready_tick(sup_ctx_t *ctx)
{
    sup_ready_t r = sup_ready_core(ctx);

    if (r.core_total == 0)
        return;
    if (r.core_ready == r.core_total) {
        if (ctx->ready_said)
            return;
        ctx->ready_said = 1;
        ctx->unready_said = 0;
        sup_log("INFO", "ready: all core daemons serving (%d/%d)",
                r.core_ready, r.core_total);
        return;
    }
    /* 启动宽限内不就绪属正常派生窗口，不告警；宽限过后仍不就绪才记账。
     * 曾经就绪又丢失的，属于真实倒退，立即记账不等宽限。 */
    if (ctx->unready_said)
        return;
    if (!ctx->ready_said && sup_now_ms() < ctx->ready_deadline)
        return;
    ctx->unready_said = 1;
    ctx->ready_said = 0;
    for (int i = 0; i < ctx->count; i++) {
        const sup_proc_t *p = &ctx->procs[i];
        if (p->role != SUP_ROLE_CORE || p->state == SUP_ST_RUNNING)
            continue;
        sup_log("WARN", "not ready: %s is %s (sock=%s)", p->name,
                sup_state_str(p->state), p->sock);
    }
}
