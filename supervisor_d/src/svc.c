// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc.c
 * @brief 常驻生命周期：prepare（就绪）→ step（单轮调谐+受理）→ clear（收摊）。
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
    sup_log("INFO", "up: home=%s %d daemons (core=%d aux=%d) tick=%ldms ep=%s",
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
