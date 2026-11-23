// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file ready.c
 * @brief 端到端就绪：CORE 子进程的端点全部可达才算启动成功。
 *
 * 历史缺陷（0.1.21 S-P2-03）：启动日志以「声明了几个 daemon」当成功，
 * 而此刻依赖往往尚未监听——运维看到 up，拿到的却是 PEP 连不上 notify_d、
 * 推理链探活 http=0 的半可用系统。
 *
 * 判据与 reconcile 同源，不引入第二套健康口径：进程存活由 reap 层
 * （waitpid/WaitForSingleObject）证明，端点可达由 sup_health_tick 的
 * sock 探测证明，二者齐备时状态机才置 RUNNING。因此「全部 CORE 处于
 * RUNNING」即「声明的依赖都已在服务」。AUX 是按需集，不计入启动判据。
 *
 * 状态迁移只在边沿发声：进入就绪记一条 INFO，丢失就绪记一次逐项缺口
 * （启动宽限内不报，避免把正常的派生窗口误报成故障），此后不刷屏。
 */

#include "supervisor_d_internal.h"

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
