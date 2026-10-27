/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_swap.c
 * @brief 配置热切换的排空闸门与代际实现（见 svc_swap.h）。
 *
 * 取号、归还、排空、提交共用 s->lock，等待在 s->cv 上完成。draining 期间
 * 取号被拒，在途计数只减不增，故排空条件单调，等待循环无需额外状态；
 * 每轮按剩余预算重算超时，spurious wakeup 不会提前判定超时。
 *
 * 世代回绕跳过 0：0 保留为「未取号」，避免回绕后与未初始化值同形。
 */

#include "svc_swap.h"

#include "airy_memory_inline.h"
#include "error.h"
#include "platform_misc.h"

int svc_swap_init(svc_swap_t *s, uint32_t drain_ms)
{
    if (!s)
        return AIRY_ERR_INVALID_PARAM;

    AIRY_MEMSET(s, 0, sizeof(*s));
    s->drain_ms = drain_ms ? drain_ms : SVC_SWAP_DRAIN_MS;
    s->gen = 1U;

    if (airy_mtx_init(&s->lock) != 0)
        return AIRY_ERR_STATE_ERROR;
    if (airy_cond_init(&s->cv) != 0) {
        airy_mtx_destroy(&s->lock);
        return AIRY_ERR_STATE_ERROR;
    }
    return 0;
}

void svc_swap_done(svc_swap_t *s)
{
    if (!s)
        return;
    airy_cond_destroy(&s->cv);
    airy_mtx_destroy(&s->lock);
}

int svc_swap_enter(svc_swap_t *s, uint32_t *gen)
{
    if (!s || !gen)
        return AIRY_ERR_INVALID_PARAM;

    int rc = 0;
    airy_mtx_lock(&s->lock);
    if (s->draining) {
        rc = AIRY_ERR_BUSY;
    } else {
        *gen = s->gen;
        s->inflight++;
    }
    airy_mtx_unlock(&s->lock);
    return rc;
}

int svc_swap_leave(svc_swap_t *s, uint32_t gen)
{
    if (!s)
        return 0;

    int ok = 0;
    airy_mtx_lock(&s->lock);
    ok = (gen != 0U && gen == s->gen);
    if (!ok)
        s->mismatch++;
    if (s->inflight > 0)
        s->inflight--;
    if (s->draining && s->inflight == 0)
        airy_cond_broadcast(&s->cv);
    airy_mtx_unlock(&s->lock);
    return ok;
}

int svc_swap_begin(svc_swap_t *s)
{
    if (!s)
        return AIRY_ERR_INVALID_PARAM;

    int rc = 0;
    airy_mtx_lock(&s->lock);
    if (s->draining) {
        airy_mtx_unlock(&s->lock);
        return AIRY_ERR_BUSY;
    }
    s->draining = 1;

    uint64_t deadline = airy_time_ms() + (uint64_t)s->drain_ms;
    while (s->inflight > 0) {
        uint64_t now = airy_time_ms();
        if (now >= deadline) {
            s->drain_tmo++;
            rc = 1;
            break;
        }
        (void)airy_cond_timedwait(&s->cv, &s->lock, (uint32_t)(deadline - now));
    }
    airy_mtx_unlock(&s->lock);
    return rc;
}

int svc_swap_commit(svc_swap_t *s)
{
    if (!s)
        return AIRY_ERR_INVALID_PARAM;

    int rc = 0;
    airy_mtx_lock(&s->lock);
    if (!s->draining) {
        rc = AIRY_ERR_STATE_ERROR;
    } else {
        s->gen++;
        if (s->gen == 0U)
            s->gen = 1U;
        s->swapped++;
        s->draining = 0;
        airy_cond_broadcast(&s->cv);
    }
    airy_mtx_unlock(&s->lock);
    return rc;
}

void svc_swap_abort(svc_swap_t *s)
{
    if (!s)
        return;

    airy_mtx_lock(&s->lock);
    s->draining = 0;
    airy_cond_broadcast(&s->cv);
    airy_mtx_unlock(&s->lock);
}

void svc_swap_stat(const svc_swap_t *s, uint32_t *gen, uint32_t *inflight, uint32_t *swapped,
                   uint64_t *mismatch, uint64_t *drain_tmo)
{
    if (!s)
        return;

    /* 只读快照仍需锁：64 位计数在 32 位目标上会被撕裂读取 */
    airy_mtx_t *lock = (airy_mtx_t *)&s->lock;
    airy_mtx_lock(lock);
    if (gen)
        *gen = s->gen;
    if (inflight)
        *inflight = s->inflight;
    if (swapped)
        *swapped = s->swapped;
    if (mismatch)
        *mismatch = s->mismatch;
    if (drain_tmo)
        *drain_tmo = s->drain_tmo;
    airy_mtx_unlock(lock);
}
