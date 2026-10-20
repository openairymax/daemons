/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_health.c
 * @brief 端点健康面状态迁移与锁内包装（0.1.19 §3.8.2）。
 *
 * 迁移判据只依赖 llm_fo_reason()，与切换决策件共用同一错误分类，
 * 避免「可切换却不算失败」「不可切换却熔断」两套口径并存。
 */

#include "router/core/llm_health.h"

#include "router/core/llm_failover.h"
#include "router/core/router_context.h"

#include "error_codes.h"
#include "platform_misc.h"

#include <string.h>

/* 开启熔断。截止时刻折零时抬为 1，保证哨兵 0 唯一表示 CLOSED。 */
static void ep_open(llm_endpoint_t *ep, uint32_t now)
{
    uint32_t until = now + LLM_EP_OPEN_MS;

    ep->open_until_ms = until != 0U ? until : 1U;
    ep->probe_until_ms = 0U;
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

void llm_ep_fail(llm_endpoint_t *ep, int err, uint32_t now)
{
    llm_fo_reason_t reason = llm_fo_reason(err);

    if (reason == LLM_FO_NONE)
        return;

    if (ep->consec_fail < UINT32_MAX)
        ep->consec_fail++;
    ep->last_fail_ms = now;

    if (reason == LLM_FO_AUTH || ep->consec_fail >= LLM_EP_FAIL_MAX) {
        ep_open(ep, now);
        AIRY_LOG_WARN("C-L02: LLMRouter: endpoint %s/%s circuit OPEN (reason=%s, fails=%u)",
                      ep->provider_name, ep->model_name, llm_fo_name(reason), ep->consec_fail);
    }
}

void llm_ep_ok(llm_endpoint_t *ep)
{
    ep->consec_fail = 0U;
    ep->last_fail_ms = 0U;
    ep->open_until_ms = 0U;
    ep->probe_until_ms = 0U;
}

static llm_endpoint_t *ep_find(const char *provider, const char *model)
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
    ep = ep_find(provider, model);
    if (ep)
        ok = llm_ep_admit(ep, now);
    AIRY_MUTEX_UNLOCK(&ctx->mutex);
    return ok;
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
    ep = ep_find(provider, model);
    if (ep) {
        if (ok)
            llm_ep_ok(ep);
        else
            llm_ep_fail(ep, err, now);
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
