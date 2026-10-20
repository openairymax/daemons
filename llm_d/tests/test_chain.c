// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_chain.c
 * @brief 0.1.19 §3.8.4 A3 调用级链式切换回归
 *
 * 锁定六条容易在后续重构中回退的口径：
 *   1. mode=off 时失败逐字节透传、只尝试一次、不写健康面
 *   2. 白名单三值可切，BAD_REQUEST 永不切，掩码剔除即不切
 *   3. 跳数上限与调用级预算共同约束总尝试数，预算比较抗 64 位回绕
 *   4. 尝试集合去重且有界；候选按优先级降序、成本升序排列
 *   5. 健康面 OPEN 的端点零尝试；未注册端点按可用处理而非拒绝
 *   6. 切换成功后落点按实际模型回填，供缓存键与计费按实际模型记账
 *
 * 真实调用以 attempt 闭包注入，不引入任何生产桩：机制件只认闭包，
 * 测试替身仅在测试进程内存在。
 */

#include "rpc/service.h"
#include "rpc/service_chain.h"

#include "providers/core/registry.h"
#include "router/core/llm_failover.h"
#include "router/core/llm_router.h"

#include "error_codes.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int test_count;
static int fail_count;

/* 适配器 fail-closed：无 base_url 不装配 provider。测试不发真实请求，
 * 该基址只用于满足装配前置条件。 */
#define TEST_API_BASE "http://127.0.0.1:1/v1"

static void check_int(const char *name, int got, int want)
{
    test_count++;
    if (got != want) {
        printf("    FAIL: %s: got %d, want %d\n", name, got, want);
        fail_count++;
    }
}

static void check_str(const char *name, const char *got, const char *want)
{
    test_count++;
    if (strcmp(got, want) != 0) {
        printf("    FAIL: %s: got \"%s\", want \"%s\"\n", name, got, want);
        fail_count++;
    }
}

static llm_endpoint_t make_ep(const char *prov, const char *model, int prio, double cost)
{
    llm_endpoint_t ep;

    memset(&ep, 0, sizeof(ep));
    snprintf(ep.provider_name, sizeof(ep.provider_name), "%s", prov);
    snprintf(ep.model_name, sizeof(ep.model_name), "%s", model);
    ep.enabled = true;
    ep.priority = prio;
    ep.context_window = 8192;
    ep.cost_per_1k_input = cost;
    ep.cost_per_1k_output = cost;
    ep.capabilities = LLM_CAP_CHAT | LLM_CAP_COMPLETION;
    return ep;
}

static llm_route_request_t make_req(void)
{
    llm_route_request_t req;

    memset(&req, 0, sizeof(req));
    req.prompt = "hello";
    req.prompt_len = 5;
    req.required_caps = LLM_CAP_CHAT | LLM_CAP_COMPLETION;
    req.strategy = LLM_ROUTE_COST;
    return req;
}

typedef struct {
    int calls;
    int fail_first;
    int err;
    int sent;
    char prov[64];
    char model[128];
} try_t;

static int fake_try(void *ctx, const svc_step_t *step, llm_response_t **out)
{
    try_t *t = (try_t *)ctx;

    (void)out;
    t->calls++;
    if (t->fail_first > 0) {
        t->fail_first--;
        return t->err;
    }
    snprintf(t->prov, sizeof(t->prov), "%s", step->prov->name ? step->prov->name : "?");
    snprintf(t->model, sizeof(t->model), "%s", step->eff.model ? step->eff.model : "");
    return 0;
}

static int fake_sent(void *ctx)
{
    return ((try_t *)ctx)->sent;
}

/* 以公开注册表工厂重建 service 的注册表，使候选可解析到 provider_t。
 * 注册表内容由 provider_config_t 描述，生命周期随 service 销毁。 */
static void reg_swap(llm_service_t *svc, provider_config_t *pcs, size_t n)
{
    service_config_t cfg;
    provider_registry_t *old;

    memset(&cfg, 0, sizeof(cfg));
    cfg.providers = pcs;
    cfg.provider_count = n;

    old = svc->registry;
    svc->registry = provider_registry_create(&cfg);
    provider_registry_destroy(old);
}

static llm_service_t *make_svc(void)
{
    llm_service_t *svc = llm_service_create(NULL);

    test_count++;
    if (!svc) {
        printf("    FAIL: llm_service_create returned NULL\n");
        fail_count++;
    }
    return svc;
}

/* 缺省值与空指针安全：off / 2 跳 / 三类白名单 / 不限预算。 */
static void test_def(void)
{
    llm_route_cfg_t cfg;
    svc_chain_t chain;

    memset(&cfg, 0, sizeof(cfg));
    svc_chain_def(&cfg);
    check_int("def.mode", cfg.mode, SVC_CHAIN_OFF);
    check_int("def.depth", cfg.depth, SVC_CHAIN_DEF_DEPTH);
    check_int("def.on", (int)cfg.on_mask, (int)LLM_FO_ON_DEFAULT);
    check_int("def.budget", cfg.budget_ms, 0);

    svc_chain_init(&chain, NULL);
    check_int("init.mode", chain.cfg.mode, SVC_CHAIN_OFF);
    check_int("init.depth", chain.cfg.depth, SVC_CHAIN_DEF_DEPTH);
    check_int("init.tried", chain.tried_n, 0);
    check_int("init.hops", chain.hops, 0);

    check_int("null.left", svc_chain_left(NULL, 0), 1);
    check_int("null.auto", svc_chain_auto(NULL), 0);
    check_int("null.retry", svc_chain_retry(NULL, AIRY_ERR_IO), 0);
    check_int("null.next", svc_chain_next(NULL, NULL, NULL, 0, NULL, 0), 0);
}

/* 预算判定：不限恒有余量，到期即耗尽，64 位回绕不误判。 */
static void test_budget(void)
{
    llm_route_cfg_t cfg;
    svc_chain_t chain;

    svc_chain_def(&cfg);
    cfg.budget_ms = 100;
    svc_chain_init(&chain, &cfg);
    chain.start_ms = 1000;

    check_int("budget.left0", svc_chain_left(&chain, 1000), 1);
    check_int("budget.left99", svc_chain_left(&chain, 1099), 1);
    check_int("budget.left100", svc_chain_left(&chain, 1100), 0);

    cfg.budget_ms = 0;
    svc_chain_init(&chain, &cfg);
    check_int("budget.unlimited", svc_chain_left(&chain, UINT64_MAX), 1);

    cfg.budget_ms = 200;
    svc_chain_init(&chain, &cfg);
    chain.start_ms = UINT64_MAX - 50U;
    check_int("budget.wrap", svc_chain_left(&chain, chain.start_ms + 100U), 1);
    check_int("budget.wrap_late", svc_chain_left(&chain, chain.start_ms + 200U), 0);
}

/* 尝试集合：同名同模型去重，满上界后静默丢弃。 */
static void test_mark(void)
{
    llm_route_cfg_t cfg;
    svc_chain_t chain;
    char prov[32];
    char model[32];

    svc_chain_def(&cfg);
    svc_chain_init(&chain, &cfg);

    svc_chain_mark(&chain, "pa", "ma");
    svc_chain_mark(&chain, "pa", "ma");
    check_int("mark.dedup", chain.tried_n, 1);

    for (int i = 0; i < SVC_CHAIN_MAX_TRIED + 4; i++) {
        snprintf(prov, sizeof(prov), "p%d", i);
        snprintf(model, sizeof(model), "m%d", i);
        svc_chain_mark(&chain, prov, model);
    }
    check_int("mark.bound", chain.tried_n, SVC_CHAIN_MAX_TRIED);

    svc_chain_mark(&chain, NULL, "mx");
    check_int("mark.null", chain.tried_n, SVC_CHAIN_MAX_TRIED);
}

/* 决策件接入：off 记首错不切；白名单三值可切；跳数/预算约束生效。 */
static void test_retry(void)
{
    llm_route_cfg_t cfg;
    svc_chain_t chain;

    svc_chain_def(&cfg);
    svc_chain_init(&chain, &cfg);
    check_int("retry.off", svc_chain_retry(&chain, AIRY_ERR_IO), 0);
    check_int("retry.off_err", chain.first_err, AIRY_ERR_IO);

    cfg.mode = SVC_CHAIN_AUTO;
    svc_chain_init(&chain, &cfg);
    check_int("retry.io", svc_chain_retry(&chain, AIRY_ERR_IO), 1);
    check_int("retry.rate", svc_chain_retry(&chain, AIRY_ERR_LLM_RATE_LIMIT), 1);
    check_int("retry.auth", svc_chain_retry(&chain, AIRY_ERR_LLM_AUTH_FAIL), 1);
    check_int("retry.first_kept", chain.first_err, AIRY_ERR_IO);
    check_int("retry.bad", svc_chain_retry(&chain, AIRY_ERR_LLM_BAD_REQUEST), 0);
    check_int("retry.first_kept2", chain.first_err, AIRY_ERR_IO);

    cfg.on_mask = LLM_FO_BIT(LLM_FO_AUTH);
    svc_chain_init(&chain, &cfg);
    check_int("retry.mask_rate", svc_chain_retry(&chain, AIRY_ERR_LLM_RATE_LIMIT), 0);
    check_int("retry.mask_auth", svc_chain_retry(&chain, AIRY_ERR_LLM_AUTH_FAIL), 1);

    cfg.on_mask = LLM_FO_ON_DEFAULT;
    cfg.depth = 1;
    svc_chain_init(&chain, &cfg);
    chain.hops = 1;
    check_int("retry.depth", svc_chain_retry(&chain, AIRY_ERR_IO), 0);

    cfg.depth = 2;
    cfg.budget_ms = 10;
    svc_chain_init(&chain, &cfg);
    chain.start_ms = 0;
    check_int("retry.budget", svc_chain_retry(&chain, AIRY_ERR_IO), 0);
}

/* 候选枚举：优先级降序、成本升序；已试跳过；命中即消耗一跳。 */
static void test_next(void)
{
    llm_endpoint_t pa = make_ep("pa", "ma", 1, 0.001);
    llm_endpoint_t pb = make_ep("pb", "mb", 2, 0.002);
    llm_endpoint_t pc = make_ep("pc", "mc", 2, 0.001);
    llm_route_request_t req = make_req();
    llm_route_cfg_t cfg;
    svc_chain_t chain;
    char prov[64];
    char model[64];

    check_int("next.init", llm_router_init(), 0);
    check_int("next.reg_a", llm_router_register_endpoint(&pa), 0);
    check_int("next.reg_b", llm_router_register_endpoint(&pb), 0);
    check_int("next.reg_c", llm_router_register_endpoint(&pc), 0);

    svc_chain_def(&cfg);
    cfg.mode = SVC_CHAIN_AUTO;
    cfg.depth = 5;
    svc_chain_init(&chain, &cfg);

    check_int("next.one", svc_chain_next(&req, &chain, prov, sizeof(prov), model, sizeof(model)), 1);
    check_str("next.cheap_first", model, "mc");
    check_int("next.two", svc_chain_next(&req, &chain, prov, sizeof(prov), model, sizeof(model)), 1);
    check_str("next.prio_second", model, "mb");
    check_int("next.three", svc_chain_next(&req, &chain, prov, sizeof(prov), model, sizeof(model)), 1);
    check_str("next.last", model, "ma");
    check_int("next.none", svc_chain_next(&req, &chain, prov, sizeof(prov), model, sizeof(model)), 0);
    check_int("next.hops", chain.hops, 3);

    svc_chain_init(&chain, &cfg);
    svc_chain_mark(&chain, "pc", "mc");
    check_int("next.skip", svc_chain_next(&req, &chain, prov, sizeof(prov), model, sizeof(model)), 1);
    check_str("next.skip_model", model, "mb");

    llm_router_destroy();
}

/* off：失败逐字节透传，只尝试一次，且不写健康面。 */
static void test_run_off(void)
{
    llm_service_t *svc = make_svc();
    llm_endpoint_t ep = make_ep("pa", "ma", 1, 0.001);
    llm_route_request_t req = make_req();
    llm_request_config_t mgr;
    provider_t prov;
    try_t t;
    llm_response_t *out = NULL;
    svc_hit_t hit;
    int rc;

    if (!svc)
        return;
    memset(&mgr, 0, sizeof(mgr));
    mgr.model = "ma";
    memset(&prov, 0, sizeof(prov));
    prov.name = "pa";
    memset(&t, 0, sizeof(t));
    t.fail_first = 1;
    t.err = AIRY_ERR_IO;
    memset(&hit, 0, sizeof(hit));

    check_int("off.reg", llm_router_register_endpoint(&ep), 0);
    rc = svc_chain_run(svc, &mgr, &req, &prov, fake_try, &t, NULL, &out, &hit);
    check_int("off.err", rc, AIRY_ERR_IO);
    check_int("off.calls", t.calls, 1);
    check_int("off.out", out == NULL, 1);
    check_int("off.health_untouched", llm_rtr_admit("pa", "ma"), 1);

    llm_service_destroy(svc);
}

/* auto：首个端点失败后切换到次候选，落点回填实际模型（H8 记账前提）。 */
static void test_run_switch(void)
{
    char *ma[] = {"ma", NULL};
    char *mb[] = {"mb", NULL};
    int cap[] = {0};
    provider_config_t pcs[2];
    llm_service_t *svc = make_svc();
    llm_endpoint_t pa = make_ep("pa", "ma", 2, 0.0001);
    llm_endpoint_t pb = make_ep("pb", "mb", 1, 0.0002);
    llm_route_request_t req = make_req();
    llm_request_config_t mgr;
    const provider_t *first;
    try_t t;
    llm_response_t *out = NULL;
    svc_hit_t hit;
    int rc;

    if (!svc)
        return;
    memset(pcs, 0, sizeof(pcs));
    pcs[0].name = "pa";
    pcs[0].api_base = TEST_API_BASE;
    pcs[0].models = ma;
    pcs[0].model_max_output = cap;
    pcs[0].timeout_sec = 1;
    pcs[1].name = "pb";
    pcs[1].api_base = TEST_API_BASE;
    pcs[1].models = mb;
    pcs[1].model_max_output = cap;
    pcs[1].timeout_sec = 1;
    reg_swap(svc, pcs, 2);

    check_int("switch.reg_a", llm_router_register_endpoint(&pa), 0);
    check_int("switch.reg_b", llm_router_register_endpoint(&pb), 0);
    svc->route.mode = SVC_CHAIN_AUTO;
    svc->route.depth = 2;

    memset(&mgr, 0, sizeof(mgr));
    mgr.model = "ma";
    memset(&t, 0, sizeof(t));
    t.fail_first = 1;
    t.err = AIRY_ERR_IO;
    memset(&hit, 0, sizeof(hit));

    first = svc_find_prov(svc, "ma");
    check_int("switch.first", first != NULL, 1);
    if (!first) {
        llm_service_destroy(svc);
        return;
    }

    rc = svc_chain_run(svc, &mgr, &req, first, fake_try, &t, NULL, &out, &hit);
    check_int("switch.ok", rc, AIRY_OK);
    check_int("switch.calls", t.calls, 2);
    check_str("switch.prov", t.prov, "pb");
    check_str("switch.model", t.model, "mb");
    check_str("switch.hit_model", hit.model, "mb");
    check_int("switch.hit_prov", hit.prov && strcmp(hit.prov->name, "pb") == 0, 1);
    check_int("switch.out", out == NULL, 1);

    llm_service_destroy(svc);
}

/* auto：候选均不可解析时不死循环，返回首个原始错误。 */
static void test_run_exhaust(void)
{
    llm_service_t *svc = make_svc();
    llm_endpoint_t ep = make_ep("pa", "ma", 1, 0.001);
    llm_route_request_t req = make_req();
    llm_request_config_t mgr;
    provider_t prov;
    try_t t;
    llm_response_t *out = NULL;
    int rc;

    if (!svc)
        return;
    memset(&mgr, 0, sizeof(mgr));
    mgr.model = "ma";
    memset(&prov, 0, sizeof(prov));
    prov.name = "pa";
    memset(&t, 0, sizeof(t));
    t.fail_first = 3;
    t.err = AIRY_ERR_LLM_RATE_LIMIT;

    check_int("exhaust.reg", llm_router_register_endpoint(&ep), 0);
    svc->route.mode = SVC_CHAIN_AUTO;
    svc->route.depth = 3;

    rc = svc_chain_run(svc, &mgr, &req, &prov, fake_try, &t, NULL, &out, NULL);
    check_int("exhaust.err", rc, AIRY_ERR_LLM_RATE_LIMIT);
    check_int("exhaust.calls", t.calls, 1);
    check_int("exhaust.out", out == NULL, 1);

    llm_service_destroy(svc);
}

/* auto：健康面 OPEN 的端点零尝试，且不把拒绝伪装成调用失败。 */
static void test_run_gate(void)
{
    llm_service_t *svc = make_svc();
    llm_endpoint_t ep = make_ep("pa", "ma", 1, 0.001);
    llm_route_request_t req = make_req();
    llm_request_config_t mgr;
    provider_t prov;
    try_t t;
    llm_response_t *out = NULL;
    int rc;

    if (!svc)
        return;
    memset(&mgr, 0, sizeof(mgr));
    mgr.model = "ma";
    memset(&prov, 0, sizeof(prov));
    prov.name = "pa";
    memset(&t, 0, sizeof(t));
    t.err = AIRY_ERR_IO;

    check_int("gate.reg", llm_router_register_endpoint(&ep), 0);
    check_int("gate.open", llm_rtr_fail("pa", "ma", AIRY_ERR_LLM_AUTH_FAIL), 0);
    svc->route.mode = SVC_CHAIN_AUTO;
    svc->route.depth = 2;

    rc = svc_chain_run(svc, &mgr, &req, &prov, fake_try, &t, NULL, &out, NULL);
    check_int("gate.err", rc, AIRY_ERR_LLM_INVALID_MODEL);
    check_int("gate.calls", t.calls, 0);
    check_int("gate.out", out == NULL, 1);

    llm_service_destroy(svc);
}

/* auto：首字节已下发后禁止切换，避免下游收到两段互不相接的内容。 */
static void test_run_sent(void)
{
    char *ma[] = {"ma", NULL};
    char *mb[] = {"mb", NULL};
    int cap[] = {0};
    provider_config_t pcs[2];
    llm_service_t *svc = make_svc();
    llm_endpoint_t pa = make_ep("pa", "ma", 2, 0.0001);
    llm_endpoint_t pb = make_ep("pb", "mb", 1, 0.0002);
    llm_route_request_t req = make_req();
    llm_request_config_t mgr;
    const provider_t *first;
    try_t t;
    llm_response_t *out = NULL;
    int rc;

    if (!svc)
        return;
    memset(pcs, 0, sizeof(pcs));
    pcs[0].name = "pa";
    pcs[0].api_base = TEST_API_BASE;
    pcs[0].models = ma;
    pcs[0].model_max_output = cap;
    pcs[1].name = "pb";
    pcs[1].api_base = TEST_API_BASE;
    pcs[1].models = mb;
    pcs[1].model_max_output = cap;
    reg_swap(svc, pcs, 2);

    check_int("sent.reg_a", llm_router_register_endpoint(&pa), 0);
    check_int("sent.reg_b", llm_router_register_endpoint(&pb), 0);
    svc->route.mode = SVC_CHAIN_AUTO;
    svc->route.depth = 2;

    memset(&mgr, 0, sizeof(mgr));
    mgr.model = "ma";
    memset(&t, 0, sizeof(t));
    t.fail_first = 1;
    t.err = AIRY_ERR_TIMEOUT;
    t.sent = 1;

    first = svc_find_prov(svc, "ma");
    check_int("sent.first", first != NULL, 1);
    if (!first) {
        llm_service_destroy(svc);
        return;
    }

    rc = svc_chain_run(svc, &mgr, &req, first, fake_try, &t, fake_sent, &out, NULL);
    check_int("sent.err", rc, AIRY_ERR_TIMEOUT);
    check_int("sent.calls", t.calls, 1);
    check_int("sent.out", out == NULL, 1);

    llm_service_destroy(svc);
}

/* 成功路径同样回填实际模型与输出上限，供截断提示与计费使用。 */
static void test_run_hit(void)
{
    char *ma[] = {"ma", NULL};
    int cap[] = {4096};
    provider_config_t pcs[1];
    llm_service_t *svc = make_svc();
    llm_endpoint_t ep = make_ep("pa", "ma", 1, 0.001);
    llm_route_request_t req = make_req();
    llm_request_config_t mgr;
    const provider_t *first;
    try_t t;
    llm_response_t *out = NULL;
    svc_hit_t hit;
    int rc;

    if (!svc)
        return;
    memset(pcs, 0, sizeof(pcs));
    pcs[0].name = "pa";
    pcs[0].api_base = TEST_API_BASE;
    pcs[0].models = ma;
    pcs[0].model_max_output = cap;
    reg_swap(svc, pcs, 1);

    check_int("hit.reg", llm_router_register_endpoint(&ep), 0);
    memset(&mgr, 0, sizeof(mgr));
    mgr.model = "ma";
    memset(&t, 0, sizeof(t));
    memset(&hit, 0, sizeof(hit));

    first = svc_find_prov(svc, "ma");
    check_int("hit.first", first != NULL, 1);
    if (!first) {
        llm_service_destroy(svc);
        return;
    }

    rc = svc_chain_run(svc, &mgr, &req, first, fake_try, &t, NULL, &out, &hit);
    check_int("hit.ok", rc, AIRY_OK);
    check_int("hit.calls", t.calls, 1);
    check_str("hit.model", hit.model, "ma");
    check_int("hit.max", hit.max_tokens, 4096);
    check_int("hit.out", out == NULL, 1);

    llm_service_destroy(svc);
}

/* 入参校验：缺任何一件都不进入尝试，输出先置空避免悬垂。 */
static void test_run_args(void)
{
    llm_route_request_t req = make_req();
    provider_t prov;
    try_t t;
    llm_response_t *out = (llm_response_t *)0x1;

    memset(&prov, 0, sizeof(prov));
    prov.name = "pa";
    memset(&t, 0, sizeof(t));

    check_int("args.svc", svc_chain_run(NULL, NULL, &req, &prov, fake_try, &t, NULL, &out, NULL),
              AIRY_ERR_INVALID_PARAM);
    check_int("args.out_null", out == NULL, 1);
    check_int("args.calls", t.calls, 0);
}

int main(void)
{
    printf("running call-chain failover regression\n");

    test_def();
    test_budget();
    test_mark();
    test_retry();
    test_next();
    test_run_off();
    test_run_switch();
    test_run_exhaust();
    test_run_gate();
    test_run_sent();
    test_run_hit();
    test_run_args();

    if (fail_count == 0) {
        printf("All %d chain assertions passed\n", test_count);
        return 0;
    }
    printf("%d/%d chain assertions failed\n", fail_count, test_count);
    return 1;
}
