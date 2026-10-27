/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_health.c
 * @brief 端点健康面状态迁移与锁内包装（0.1.19 §3.8.2，0.1.20 D2-3）。
 *
 * 迁移判据只依赖 llm_fo_reason()，与切换决策件共用同一错误分类，
 * 避免「可切换却不算失败」「不可切换却熔断」两套口径并存。
 *
 * 0.1.20 补两处质量缺口：
 *   - CLOSED 期的 consec_fail 满 LLM_EP_FAIL_WINDOW_MS 无新失败即衰减，
 *     使稀疏偶发失败不再跨天累计成熔断；
 *   - HALF 期连续探测失败达 LLM_EP_PROBE_MAX 才回 OPEN，且每轮往返抬一档
 *     指数退避，持续故障端点的探测频率随时间下降，不再无限 10s 一撞。
 */

#include "router/core/llm_failover.h"
#include "router/core/router_context.h"
#include "router/core/router_state.h"

#include "error_codes.h"
#include "platform_misc.h"

#include <string.h>

/* 开启熔断。截止时刻折零时抬为 1，保证哨兵 0 唯一表示 CLOSED。
 * 保持期按 open_level 指数抬升：首档 LLM_EP_OPEN_MS，每轮翻倍，
 * 封顶 LLM_EP_OPEN_MAX_MS。 */
static void ep_open(llm_endpoint_t *ep, uint32_t now)
{
    uint32_t level = ep->open_level < LLM_EP_MAX_LEVEL ? ep->open_level : LLM_EP_MAX_LEVEL;
    uint64_t hold = (uint64_t)LLM_EP_OPEN_MS << level;
    uint32_t until;

    if (hold > LLM_EP_OPEN_MAX_MS)
        hold = LLM_EP_OPEN_MAX_MS;
    until = now + (uint32_t)hold;

    ep->open_until_ms = until != 0U ? until : 1U;
    ep->probe_until_ms = 0U;
    if (ep->open_level < UINT8_MAX)
        ep->open_level++;
}

llm_ep_state_t llm_ep_state(const llm_endpoint_t *ep, uint32_t now)
{
    if (ep->open_until_ms == 0U)
        return LLM_EP_CLOSED;
    if ((int32_t)(ep->open_until_ms - now) > 0)
        return LLM_EP_OPEN;
    return LLM_EP_HALF;
}

int llm_ep_admit(llm_endpoint_t *ep, uint32_t now)
{
    llm_ep_state_t state = llm_ep_state(ep, now);
    uint32_t until;

    if (state == LLM_EP_OPEN)
        return 0;
    if (state == LLM_EP_CLOSED)
        return 1;
    if (ep->probe_until_ms != 0U && (int32_t)(ep->probe_until_ms - now) > 0)
        return 0;

    until = now + LLM_EP_PROBE_MS;
    ep->probe_until_ms = until != 0U ? until : 1U;
    return 1;
}

/* CLOSED 期失败计数：满窗无新失败则重新起算，再由本次失败起新一轮。 */
static void ep_count(llm_endpoint_t *ep, uint32_t now)
{
    if (ep->consec_fail != 0U && (uint32_t)(now - ep->last_fail_ms) >= LLM_EP_FAIL_WINDOW_MS)
        ep->consec_fail = 0U;
    if (ep->consec_fail < UINT32_MAX)
        ep->consec_fail++;
    ep->last_fail_ms = now;
}

void llm_ep_fail(llm_endpoint_t *ep, int err, uint32_t now)
{
    llm_fo_reason_t reason = llm_fo_reason(err);
    llm_ep_state_t state;

    if (reason == LLM_FO_NONE)
        return;

    state = llm_ep_state(ep, now);

    /* 熔断期内到达的失败只更新时间戳，不延长保持期：否则持续打点会把
     * 端点永久钉在 OPEN，退避档位也失去意义。 */
    if (state == LLM_EP_OPEN) {
        ep->last_fail_ms = now;
        return;
    }

    if (state == LLM_EP_HALF) {
        ep->last_fail_ms = now;
        if (reason == LLM_FO_AUTH || ++ep->probe_fail >= LLM_EP_PROBE_MAX) {
            ep->probe_fail = 0U;
            ep_open(ep, now);
            AIRY_LOG_WARN("C-L02: LLMRouter: endpoint %s/%s circuit OPEN (reason=%s, probes=%u)",
                          ep->provider_name, ep->model_name, llm_fo_name(reason),
                          (unsigned)LLM_EP_PROBE_MAX);
        }
        return;
    }

    ep_count(ep, now);
    if (reason == LLM_FO_AUTH || ep->consec_fail >= LLM_EP_FAIL_MAX) {
        ep->probe_fail = 0U;
        ep_open(ep, now);
        AIRY_LOG_WARN("C-L02: LLMRouter: endpoint %s/%s circuit OPEN (reason=%s, fails=%u)",
                      ep->provider_name, ep->model_name, llm_fo_name(reason),
                      (unsigned)ep->consec_fail);
    }
}

void llm_ep_ok(llm_endpoint_t *ep)
{
    ep->consec_fail = 0U;
    ep->last_fail_ms = 0U;
    ep->open_until_ms = 0U;
    ep->probe_until_ms = 0U;
    ep->probe_fail = 0U;
    ep->open_level = 0U;
}

llm_endpoint_t *router_ep_find(const char *provider, const char *model)
{
    router_ctx_t *ctx = router_ctx_get();

    for (size_t i = 0; i < ctx->endpoint_count; i++) {
        llm_endpoint_t *ep = &ctx->endpoints[i];
        if (strcmp(ep->provider_name, provider) == 0 && strcmp(ep->model_name, model) == 0)
            return ep;
    }
    return NULL;
}

int llm_rtr_admit(const char *provider, const char *model)
{
    router_ctx_t *ctx = router_ctx_get();
    llm_endpoint_t *ep;
    uint32_t now;
    int ok = 0;

    if (!provider || !model || !ctx->initialized)
        return 0;

    now = (uint32_t)airy_time_ms();
    AIRY_MUTEX_LOCK(&ctx->mutex);
    ep = router_ep_find(provider, model);
    if (ep)
        ok = llm_ep_admit(ep, now);
    AIRY_MUTEX_UNLOCK(&ctx->mutex);
    return ok;
}

int llm_rtr_known(const char *provider, const char *model)
{
    router_ctx_t *ctx = router_ctx_get();
    int known;

    if (!provider || !model || !ctx->initialized)
        return 0;

    AIRY_MUTEX_LOCK(&ctx->mutex);
    known = router_ep_find(provider, model) != NULL;
    AIRY_MUTEX_UNLOCK(&ctx->mutex);
    return known;
}

/* 健康面签名：落盘与否只由这六个量决定，避免每次成功调用都写文件。 */
typedef struct {
    uint32_t consec_fail;
    uint32_t last_fail_ms;
    uint32_t open_until_ms;
    uint32_t probe_until_ms;
    uint32_t probe_fail;
    uint8_t open_level;
} ep_sig_t;

static ep_sig_t ep_sig_of(const llm_endpoint_t *ep)
{
    ep_sig_t s;

    s.consec_fail = ep->consec_fail;
    s.last_fail_ms = ep->last_fail_ms;
    s.open_until_ms = ep->open_until_ms;
    s.probe_until_ms = ep->probe_until_ms;
    s.probe_fail = ep->probe_fail;
    s.open_level = ep->open_level;
    return s;
}

static bool ep_sig_differs(const ep_sig_t *a, const ep_sig_t *b)
{
    return a->consec_fail != b->consec_fail || a->last_fail_ms != b->last_fail_ms ||
           a->open_until_ms != b->open_until_ms || a->probe_until_ms != b->probe_until_ms ||
           a->probe_fail != b->probe_fail || a->open_level != b->open_level;
}

/* 锁内推进一次健康迁移；返回真表示六个健康字段确有变化，需要落盘。
 * 落盘计划由调用方统一裁决：单端点记账立即写，provider 批量记账写一次。 */
static bool ep_step(llm_endpoint_t *ep, int ok, int err, uint32_t now)
{
    ep_sig_t before = ep_sig_of(ep);
    ep_sig_t after;

    if (ok)
        llm_ep_ok(ep);
    else
        llm_ep_fail(ep, err, now);
    after = ep_sig_of(ep);
    return ep_sig_differs(&before, &after);
}

static int ep_note(const char *provider, const char *model, int err, bool ok)
{
    router_ctx_t *ctx = router_ctx_get();
    llm_endpoint_t *ep;
    uint32_t now;
    int rc = AIRY_ERR_NOT_FOUND;

    if (!provider || !model)
        return AIRY_ERR_INVALID_PARAM;
    if (!ctx->initialized)
        return AIRY_ERR_SYS_NOT_INIT;

    now = (uint32_t)airy_time_ms();
    AIRY_MUTEX_LOCK(&ctx->mutex);
    ep = router_ep_find(provider, model);
    if (ep) {
        if (ep_step(ep, ok, err, now))
            (void)router_state_save_locked();
        rc = 0;
    }
    AIRY_MUTEX_UNLOCK(&ctx->mutex);
    return rc;
}

int llm_rtr_fail(const char *provider, const char *model, int err)
{
    return ep_note(provider, model, err, false);
}

int llm_rtr_ok(const char *provider, const char *model)
{
    return ep_note(provider, model, 0, true);
}

size_t llm_rtr_provider_note(const char *provider, int ok)
{
    router_ctx_t *ctx = router_ctx_get();
    uint32_t now;
    size_t hit = 0;
    bool dirty = false;

    if (!provider || !provider[0] || !ctx->initialized)
        return 0;

    now = (uint32_t)airy_time_ms();
    AIRY_MUTEX_LOCK(&ctx->mutex);
    for (size_t i = 0; i < ctx->endpoint_count; i++) {
        llm_endpoint_t *ep = &ctx->endpoints[i];

        if (strcmp(ep->provider_name, provider) != 0)
            continue;
        dirty |= ep_step(ep, ok, AIRY_ERR_IO, now);
        hit++;
    }
    if (dirty)
        (void)router_state_save_locked();
    AIRY_MUTEX_UNLOCK(&ctx->mutex);
    return hit;
}

const char *llm_health_name(llm_health_state_t state)
{
    switch (state) {
    case LLM_HEALTH_OK:
        return "ok";
    case LLM_HEALTH_DEGRADED:
        return "degraded";
    default:
        return "down";
    }
}

llm_health_state_t llm_rtr_health(llm_health_snap_t *out)
{
    router_ctx_t *ctx = router_ctx_get();
    llm_health_snap_t snap = {LLM_HEALTH_DOWN, 0, 0, 0};
    uint32_t now;

    if (!ctx->initialized) {
        if (out)
            *out = snap;
        return snap.state;
    }

    now = (uint32_t)airy_time_ms();
    AIRY_MUTEX_LOCK(&ctx->mutex);
    for (size_t i = 0; i < ctx->endpoint_count; i++) {
        const llm_endpoint_t *ep = &ctx->endpoints[i];

        if (!ep->enabled)
            continue;
        snap.total++;
        if (llm_ep_state(ep, now) == LLM_EP_CLOSED)
            snap.ok++;
        else
            snap.down++;
    }
    AIRY_MUTEX_UNLOCK(&ctx->mutex);

    /* 三态判据（0.1.20 J-12）：全部 CLOSED 才是 ok；无一可用即 down；
     * 其余为 degraded。无启用端点同样落 down——没有上游就不是可用。 */
    if (snap.total == 0 || snap.ok == 0)
        snap.state = LLM_HEALTH_DOWN;
    else if (snap.ok == snap.total)
        snap.state = LLM_HEALTH_OK;
    else
        snap.state = LLM_HEALTH_DEGRADED;

    if (out)
        *out = snap;
    return snap.state;
}
