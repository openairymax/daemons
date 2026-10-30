/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service_chain.c
 * @brief 调用级链式切换机制实现（0.1.19 §3.8.4 A3）。
 *
 * 循环顺序固定为：解析实际模型 → 健康面放行 → 真实调用 → 记账迁移 →
 * 决策件判定 → 取下一候选。放行在调用前、记账在调用后，两侧都以
 * provider 名与实际模型为键，与注册端点的健康面键严格同源。
 *
 * 健康面记账与模式无关：无论 off 还是 auto，真实调用成功/失败都落到端点
 * 健康面（D2-2），否则默认配置下熔断计数恒为 0、机制形同虚设。off 仅关闭
 * 放行闸、候选枚举与预算消耗，失败原样返回上层返回码，使 0.1.18 的现网
 * 选路行为逐字节保持。
 */

#include "rpc/service_chain.h"

#include "airy_memory.h"
#include "platform_misc.h"
#include "providers/core/provider_base.h"
#include "providers/core/provider_rec.h"
#include "rpc/gen_params.h"
#include "router/core/llm_failover.h"
#include "svc_logger.h"
#include "sync.h"

#include <string.h>

/* D2-9④（r7 硬化）：本地端点并发闸——
 *   server_parallel = first_nonzero(/props.total_slots, cfg, 1)
 *   max_inflight    = min(first_nonzero(cfg, 1), server_parallel)
 * 超限 AIRY_EBUSY 快速失败（链式语义下转下一候选端点）。cfg = 该模型的
 * model.yaml:max_concurrent。云族端点（未探测，props_state=0）不设闸。 */
static int svc_gate_limit(const provider_t *prov, const char *model)
{
    provider_base_ctx_t *base = provider_base_ctx(prov->ctx);
    if (!base || base->props_state == 0)
        return 0; /* 不设闸 */
    int cfg = prov_max_conc(prov, model);
    int sp = base->total_slots > 0 ? base->total_slots : (cfg > 0 ? cfg : 1);
    int mi = cfg > 0 ? cfg : 1;
    return mi < sp ? mi : sp;
}

/* 返回值：>0 = 已占座，值为闸上限（退出前须配对 release）；0 = 未设闸；
 * -1 = 在座满，本端点本轮不可用。 */
static int svc_gate_acquire(const provider_t *prov, const char *model)
{
    int limit = svc_gate_limit(prov, model);
    if (limit <= 0)
        return 0;
    provider_base_ctx_t *base = provider_base_ctx(prov->ctx);
    for (;;) {
        uintptr_t cur = sync_atomic_load(&base->inflight);
        if ((int)cur >= limit)
            return -1;
        if (sync_atomic_cas(&base->inflight, cur, cur + 1))
            return limit;
    }
}

static void svc_gate_release(const provider_t *prov)
{
    provider_base_ctx_t *base = provider_base_ctx(prov->ctx);
    if (base)
        (void)sync_atomic_sub(&base->inflight, 1);
}

void svc_chain_def(llm_route_cfg_t *cfg)
{
    if (!cfg)
        return;
    cfg->mode = SVC_CHAIN_OFF;
    cfg->depth = SVC_CHAIN_DEF_DEPTH;
    cfg->on_mask = LLM_FO_ON_DEFAULT;
    cfg->budget_ms = 0;
}

void svc_chain_init(svc_chain_t *chain, const llm_route_cfg_t *cfg)
{
    if (!chain)
        return;
    AIRY_MEMSET(chain, 0, sizeof(*chain));
    svc_chain_def(&chain->cfg);
    if (cfg)
        chain->cfg = *cfg;
    chain->start_ms = airy_time_ms();
}

int svc_chain_left(const svc_chain_t *chain, uint64_t now)
{
    if (!chain || chain->cfg.budget_ms <= 0)
        return 1;
    /* 无符号差比较：跨 2^49 毫秒的回绕也不会误判为耗尽。 */
    return (now - chain->start_ms) < (uint64_t)chain->cfg.budget_ms;
}

int svc_chain_auto(const svc_chain_t *chain)
{
    return chain && chain->cfg.mode == SVC_CHAIN_AUTO;
}

static int chain_tried(const svc_chain_t *chain, const char *prov, const char *model)
{
    for (int i = 0; i < chain->tried_n; i++) {
        if (strcmp(chain->tried[i].prov, prov) == 0 &&
            strcmp(chain->tried[i].model, model) == 0)
            return 1;
    }
    return 0;
}

void svc_chain_mark(svc_chain_t *chain, const char *prov, const char *model)
{
    if (!chain || !prov || !model || chain->tried_n >= SVC_CHAIN_MAX_TRIED)
        return;
    if (chain_tried(chain, prov, model))
        return;

    svc_ep_t *ep = &chain->tried[chain->tried_n++];
    AIRY_STRNCPY_TERM(ep->prov, prov, sizeof(ep->prov));
    AIRY_STRNCPY_TERM(ep->model, model, sizeof(ep->model));
}

int svc_chain_retry(svc_chain_t *chain, int err)
{
    if (!chain)
        return 0;
    if (chain->first_err == 0)
        chain->first_err = err;
    if (!svc_chain_auto(chain))
        return 0;
    if (!llm_fo_should(err, chain->cfg.on_mask))
        return 0;
    if (chain->hops >= chain->cfg.depth)
        return 0;
    return svc_chain_left(chain, airy_time_ms());
}

int svc_chain_next(const llm_route_request_t *req, svc_chain_t *chain, char *prov, size_t psz,
                   char *model, size_t msz)
{
    llm_route_result_t cand[SVC_CHAIN_MAX_TRIED];
    size_t n = 0;

    if (!req || !chain || !prov || !model || psz == 0 || msz == 0)
        return 0;
    /* 尝试集合有界是终止性前提：集合满则不再枚举，避免排除集合溢出后
     * 反复取回同一端点。 */
    if (chain->tried_n >= SVC_CHAIN_MAX_TRIED)
        return 0;
    if (llm_router_pick(req, cand, SVC_CHAIN_MAX_TRIED, &n) != 0)
        return 0;

    for (size_t i = 0; i < n; i++) {
        if (chain_tried(chain, cand[i].provider_name, cand[i].model_name))
            continue;
        AIRY_STRNCPY_TERM(prov, cand[i].provider_name, psz);
        AIRY_STRNCPY_TERM(model, cand[i].model_name, msz);
        svc_chain_mark(chain, cand[i].provider_name, cand[i].model_name);
        chain->hops++;
        return 1;
    }
    return 0;
}

/* 健康面放行。未注册端点无从判定，按可用处理：router 未初始化或端点
 * 超出注册上限时，若按拒绝处理会把本可尝试的端点全部跳过。 */
static int ep_gate(const provider_t *prov, const char *model)
{
    if (!prov || !prov->name)
        return 1;
    if (llm_rtr_admit(prov->name, model))
        return 1;
    return llm_rtr_known(prov->name, model) ? 0 : 1;
}

/* 取下一个候选并解析回注册表；未命中的候选已记入集合，继续向后。 */
static const provider_t *next_prov(llm_service_t *svc, const llm_route_request_t *req,
                                   svc_chain_t *chain, char *model, size_t msz)
{
    char prov[64];

    while (svc_chain_next(req, chain, prov, sizeof(prov), model, msz)) {
        const provider_t *hit = svc_find_prov(svc, model);
        if (hit)
            return hit;
        SVC_LOG_DEBUG("C-L02: SVC: CHAIN-SKIP %s/%s not in registry", prov, model);
    }
    return NULL;
}

/* 单步装配：生效配置（模型 + 生成参数）与命中报告一次成型。 */
static void step_build(llm_service_t *svc, const provider_t *prov,
                       const llm_request_config_t *manager, const char *cur_model,
                       svc_step_t *step, char *model, size_t msz, int *max_tokens, svc_hit_t *hit)
{
    llm_request_config_t eff = *manager;

    if (cur_model[0])
        eff.model = cur_model;
    resolve_gen_params(svc, prov, &eff, model, msz, max_tokens);
    eff.model = model;
    eff.max_tokens = *max_tokens;

    step->prov = prov;
    step->eff = eff;

    if (hit) {
        hit->prov = prov;
        AIRY_STRNCPY_TERM(hit->model, model, sizeof(hit->model));
        hit->max_tokens = *max_tokens;
    }
}

/* 链上放行：返回 0 = 放行（*out_gated 记录占座深度），1 = 转下一候选，
 * -1 = 座满且非 auto 模式（调用方原样回 BUSY）。 */
static int chain_gate(svc_chain_t *chain, const provider_t *prov, const char *model,
                      int auto_mode, int *out_gated)
{
    *out_gated = 0;

    if (auto_mode) {
        /* 先记集合再判放行：被健康面拒绝的端点同样不得再次入选。 */
        svc_chain_mark(chain, prov->name, model);
        if (!ep_gate(prov, model)) {
            SVC_LOG_WARN("C-L02: SVC: CHAIN-SKIP %s/%s (circuit open or probe busy)",
                         prov->name ? prov->name : "?", model);
            return 1;
        }
    }

    /* D2-9④：占座在调用前、退座在调用后，与链式放行独立——云族未设闸
     * 时两者都是 no-op。在座满不记健康面失败（这是本地容量背压，不是
     * 端点故障）：auto 模式直接转下一候选，off 模式原样回 BUSY。 */
    int gated = svc_gate_acquire(prov, model);

    if (gated < 0) {
        SVC_LOG_WARN("C-L02: SVC: CHAIN-BUSY %s/%s (inflight at cap)",
                     prov->name ? prov->name : "?", model);
        return auto_mode ? 1 : -1;
    }
    *out_gated = gated;
    return 0;
}

/* 单次尝试：退座、成功度量、失败记账一次收口。返回 attempt 的错误码，
 * 失败路径上的半成品响应在此释放，避免每次切换泄漏一次。 */
static int attempt_run(const provider_t *prov, const char *model, svc_attempt_fn attempt,
                       void *actx, svc_step_t *step, int gated, llm_response_t **resp)
{
    uint64_t t0 = airy_time_ms();
    int ret = attempt(actx, step, resp);

    if (gated > 0)
        svc_gate_release(prov);
    if (ret == 0) {
        (void)llm_rtr_ok(prov->name, model);
        (void)llm_rtr_meas(prov->name, model, *resp, airy_time_ms() - t0);
        return 0;
    }

    llm_response_free(*resp);
    *resp = NULL;
    (void)llm_rtr_fail(prov->name, model, ret);
    return ret;
}

/* 单端点推进的三态结果：成功 / 换下一候选 / 终止整条链。 */
enum { CHAIN_OK = 0, CHAIN_NEXT = 1, CHAIN_STOP = 2 };

/* 一次调用的不变量：调用方给定、循环内不改变的部分集中在此，避免把同一
 * 组实参在循环与助手之间来回搬运。 */
typedef struct {
    llm_service_t *svc;
    const llm_request_config_t *manager;
    const llm_route_request_t *req;
    svc_attempt_fn attempt;
    void *actx;
    svc_sent_fn sent;
    svc_hit_t *hit;
    char *cur_model; /* 指向调用方栈上的生效模型槽，由 next_prov 回写 */
} run_ctx_t;

static int run_args_ok(const run_ctx_t *rc, const provider_t *first)
{
    return rc && rc->svc && rc->manager && rc->req && first && rc->attempt;
}

/* 单端点推进：装配 → 放行 → 调用。成功时 *done 指向响应；失败按模式决定
 * 换候选（CHAIN_NEXT）、终止（CHAIN_STOP）或把返回码原样上抛。 */
static int chain_try(run_ctx_t *rc, svc_chain_t *chain, int auto_mode, const provider_t *prov,
                     llm_response_t **done)
{
    svc_step_t step;
    char model[128];
    int max_tokens = 0;
    int gated = 0;

    step_build(rc->svc, prov, rc->manager, rc->cur_model, &step, model, sizeof(model), &max_tokens,
               rc->hit);

    int gate = chain_gate(chain, prov, model, auto_mode, &gated);
    if (gate != 0)
        return gate < 0 ? AIRY_ERR_BUSY : CHAIN_NEXT;

    int ret = attempt_run(prov, model, rc->attempt, rc->actx, &step, gated, done);
    if (ret == 0)
        return CHAIN_OK;
    /* 记账已在 attempt_run 内完成；此处只做模式分派。 */
    if (!auto_mode)
        return ret;
    if (!svc_chain_retry(chain, ret))
        return CHAIN_STOP;
    /* 首字节已下发后再换端点会让下游收到两段互不相接的内容。 */
    return (rc->sent && rc->sent(rc->actx)) ? CHAIN_STOP : CHAIN_NEXT;
}

int svc_chain_run(llm_service_t *svc, const llm_request_config_t *manager,
                  const llm_route_request_t *req, const provider_t *first, svc_attempt_fn attempt,
                  void *actx, svc_sent_fn sent, llm_response_t **out, svc_hit_t *hit)
{
    if (out)
        *out = NULL;

    char cur_model[128] = {0};
    run_ctx_t rc = {.svc = svc,
                    .manager = manager,
                    .req = req,
                    .attempt = attempt,
                    .actx = actx,
                    .sent = sent,
                    .hit = hit,
                    .cur_model = cur_model};
    if (!run_args_ok(&rc, first))
        return AIRY_ERR_INVALID_PARAM;

    svc_chain_t chain;
    llm_route_cfg_t route;

    svc_chain_ctx(svc, &route);
    svc_chain_init(&chain, &route);

    int auto_mode = svc_chain_auto(&chain);
    const provider_t *prov = first;

    while (prov) {
        llm_response_t *resp = NULL;
        int rc2 = chain_try(&rc, &chain, auto_mode, prov, &resp);

        if (rc2 == CHAIN_OK) {
            if (out)
                *out = resp;
            else
                llm_response_free(resp);
            return AIRY_OK;
        }
        if (rc2 == CHAIN_STOP)
            break;
        if (rc2 != CHAIN_NEXT)
            return rc2;
        prov = next_prov(svc, req, &chain, cur_model, sizeof(cur_model));
    }

    return chain.first_err ? chain.first_err : AIRY_ERR_LLM_INVALID_MODEL;
}
