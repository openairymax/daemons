// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_health.c
 * @brief 0.1.19 §3.8.2 端点健康面 + 0.1.20 D2-3 质量项回归
 *
 * 锁定七条容易在重构中回退的口径：
 *   1. 连续失败达阈值才 OPEN；AUTH 一次即 OPEN
 *   2. 不可切换类错误（BAD_REQUEST / 策略拒绝）不计入健康面
 *   3. HALF 只放行单次探测，探测槽超时释放，成功归 CLOSED 并清零退避档位
 *   4. HALF 期连续探测失败达 PROBE_MAX 才回 OPEN，期间失败计数不再累加
 *   5. 每轮重回 OPEN 按 open_level 指数抬升保持期，封顶 OPEN_MAX_MS
 *   6. CLOSED 期满 FAIL_WINDOW_MS 无新失败即衰减，稀疏失败不跨天累计
 *   7. 健康面落盘后重启恢复：同 provider/model 重注册即回到 OPEN
 *   8. 端点未就绪（SVC_NOT_READY / HTTP 503）可切换但不计入健康账
 */

#include "router/core/llm_rtr_health.h"
#include "router/core/router_context.h"

#include "error_codes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_count;
static int fail_count;
static char g_root[256];

static void check_state(const char *name, const llm_endpoint_t *ep, uint32_t now,
                        llm_ep_state_t want)
{
    llm_ep_state_t got = llm_ep_state(ep, now);

    test_count++;
    if (got != want) {
        printf("    FAIL: %s: state=%d, want %d\n", name, (int)got, (int)want);
        fail_count++;
    }
}

static void check_admit(const char *name, llm_endpoint_t *ep, uint32_t now, int want)
{
    int got = llm_ep_admit(ep, now);

    test_count++;
    if (got != want) {
        printf("    FAIL: %s: admit=%d, want %d\n", name, got, want);
        fail_count++;
    }
}

static void check_int(const char *name, int got, int want)
{
    test_count++;
    if (got != want) {
        printf("    FAIL: %s: got %d, want %d\n", name, got, want);
        fail_count++;
    }
}

static void check_u32(const char *name, uint32_t got, uint32_t want)
{
    test_count++;
    if (got != want) {
        printf("    FAIL: %s: got %u, want %u\n", name, got, want);
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

static llm_endpoint_t make_ep(const char *provider, const char *model)
{
    llm_endpoint_t ep;

    memset(&ep, 0, sizeof(ep));
    snprintf(ep.provider_name, sizeof(ep.provider_name), "%s", provider);
    snprintf(ep.model_name, sizeof(ep.model_name), "%s", model);
    ep.enabled = true;
    ep.capabilities = LLM_CAP_CHAT | LLM_CAP_COMPLETION;
    return ep;
}

/* 连续失败三次转 OPEN，OPEN 期内拒绝一切调用。 */
static void test_threshold(void)
{
    llm_endpoint_t ep;
    memset(&ep, 0, sizeof(ep));

    check_state("closed_start", &ep, 100U, LLM_EP_CLOSED);
    check_admit("closed_admit", &ep, 100U, 1);

    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 100U);
    check_u32("fail_once_count", ep.consec_fail, 1U);
    check_state("fail_once_state", &ep, 100U, LLM_EP_CLOSED);

    llm_ep_fail(&ep, AIRY_ERR_IO, 200U);
    check_state("fail_twice_state", &ep, 200U, LLM_EP_CLOSED);

    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 300U);
    check_u32("fail_three_count", ep.consec_fail, LLM_EP_FAIL_MAX);
    check_u32("open_until", ep.open_until_ms, 300U + LLM_EP_OPEN_MS);
    check_u32("last_fail", ep.last_fail_ms, 300U);
    check_u32("open_level", ep.open_level, 1U);
    check_state("open_state", &ep, 300U, LLM_EP_OPEN);
    check_admit("open_denied", &ep, 300U, 0);
    check_admit("open_denied_late", &ep, 300U + LLM_EP_OPEN_MS - 1U, 0);
}

/* 熔断窗口过期转 HALF：首个调用方占探测槽，槽满拒绝，超时释放。 */
static void test_half_open(void)
{
    llm_endpoint_t ep;
    uint32_t until;

    memset(&ep, 0, sizeof(ep));
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 100U);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 100U);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 100U);
    until = ep.open_until_ms;

    check_state("half_state", &ep, until, LLM_EP_HALF);
    check_admit("half_probe", &ep, until, 1);
    check_admit("half_probe_busy", &ep, until, 0);
    check_admit("half_probe_busy_late", &ep, until + LLM_EP_PROBE_MS - 1U, 0);
    check_admit("half_probe_expired", &ep, until + LLM_EP_PROBE_MS, 1);
}

/* 探测成功归 CLOSED 并清零；连续探测失败达上限才回 OPEN，失败计数不累加。 */
static void test_probe_outcome(void)
{
    llm_endpoint_t ok_ep;
    llm_endpoint_t bad_ep;
    uint32_t until;

    memset(&ok_ep, 0, sizeof(ok_ep));
    llm_ep_fail(&ok_ep, AIRY_ERR_LLM_RATE_LIMIT, 10U);
    llm_ep_fail(&ok_ep, AIRY_ERR_LLM_RATE_LIMIT, 20U);
    llm_ep_fail(&ok_ep, AIRY_ERR_LLM_RATE_LIMIT, 30U);
    check_admit("probe_claim", &ok_ep, ok_ep.open_until_ms, 1);
    llm_ep_ok(&ok_ep);
    check_state("probe_ok_state", &ok_ep, 40U, LLM_EP_CLOSED);
    check_u32("probe_ok_count", ok_ep.consec_fail, 0U);
    check_u32("probe_ok_open", ok_ep.open_until_ms, 0U);
    check_u32("probe_ok_slot", ok_ep.probe_until_ms, 0U);
    check_u32("probe_ok_level", ok_ep.open_level, 0U);

    memset(&bad_ep, 0, sizeof(bad_ep));
    llm_ep_fail(&bad_ep, AIRY_ERR_TIMEOUT, 10U);
    llm_ep_fail(&bad_ep, AIRY_ERR_TIMEOUT, 10U);
    llm_ep_fail(&bad_ep, AIRY_ERR_TIMEOUT, 10U);
    until = bad_ep.open_until_ms;
    check_admit("probe_claim_bad", &bad_ep, until, 1);

    llm_ep_fail(&bad_ep, AIRY_ERR_TIMEOUT, until);
    check_state("probe_bad_1_state", &bad_ep, until, LLM_EP_HALF);
    check_u32("probe_bad_1_count", bad_ep.consec_fail, LLM_EP_FAIL_MAX);
    check_u32("probe_bad_1_level", bad_ep.open_level, 1U);

    llm_ep_fail(&bad_ep, AIRY_ERR_TIMEOUT, until);
    check_state("probe_bad_2_state", &bad_ep, until, LLM_EP_HALF);

    llm_ep_fail(&bad_ep, AIRY_ERR_TIMEOUT, until);
    check_state("probe_bad_3_state", &bad_ep, until, LLM_EP_OPEN);
    check_u32("probe_bad_3_until", bad_ep.open_until_ms, until + 2U * LLM_EP_OPEN_MS);
    check_u32("probe_bad_3_level", bad_ep.open_level, 2U);
    check_u32("probe_bad_3_probe", bad_ep.probe_fail, 0U);
}

/* 密钥错误逐请求重撞只会拖长故障面：一次即摘除端点；HALF 期同为一票。 */
static void test_auth(void)
{
    llm_endpoint_t ep;
    uint32_t until;

    memset(&ep, 0, sizeof(ep));
    llm_ep_fail(&ep, AIRY_ERR_LLM_AUTH_FAIL, 500U);
    check_u32("auth_count", ep.consec_fail, 1U);
    check_state("auth_open", &ep, 500U, LLM_EP_OPEN);
    check_admit("auth_denied", &ep, 500U, 0);

    memset(&ep, 0, sizeof(ep));
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 600U);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 600U);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 600U);
    until = ep.open_until_ms;
    llm_ep_fail(&ep, AIRY_ERR_LLM_AUTH_FAIL, until);
    check_state("auth_half_state", &ep, until, LLM_EP_OPEN);
    check_u32("auth_half_until", ep.open_until_ms, until + 2U * LLM_EP_OPEN_MS);
    check_u32("auth_half_level", ep.open_level, 2U);
}

/* 请求非法 / 策略拒绝不是端点故障，不得熔断健康端点。 */
static void test_ignored(void)
{
    llm_endpoint_t ep;
    uint32_t until;
    int i;

    memset(&ep, 0, sizeof(ep));
    for (i = 0; i < 5; i++) {
        llm_ep_fail(&ep, AIRY_ERR_LLM_BAD_REQUEST, 100U);
        llm_ep_fail(&ep, AIRY_ERR_LLM_CONTEXT_LEN, 100U);
        llm_ep_fail(&ep, AIRY_ERR_PERMISSION_DENIED, 100U);
    }
    check_u32("ignored_count", ep.consec_fail, 0U);
    check_u32("ignored_last", ep.last_fail_ms, 0U);
    check_u32("ignored_open", ep.open_until_ms, 0U);
    check_state("ignored_state", &ep, 100U, LLM_EP_CLOSED);

    /* 端点未就绪（冷加载/上游重启）可换端点，但不是端点故障：
     * 计入健康账会把一个正在正常启动的端点熔断掉。 */
    for (i = 0; i < 5; i++)
        llm_ep_fail(&ep, AIRY_ERR_SVC_NOT_READY, 100U);
    check_u32("not_ready_count", ep.consec_fail, 0U);
    check_u32("not_ready_open", ep.open_until_ms, 0U);
    check_state("not_ready_state", &ep, 100U, LLM_EP_CLOSED);

    /* HALF 期同理：探测收到 not_ready 不算探测失败。 */
    memset(&ep, 0, sizeof(ep));
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 700U);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 700U);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 700U);
    until = ep.open_until_ms;
    check_admit("not_ready_probe", &ep, until, 1);
    llm_ep_fail(&ep, AIRY_ERR_SVC_NOT_READY, until);
    check_state("not_ready_half_state", &ep, until, LLM_EP_HALF);
    check_u32("not_ready_half_probe", ep.probe_fail, 0U);
}

/* 稀疏失败不跨窗口累计：满窗无新失败即重新起算。 */
static void test_decay(void)
{
    llm_endpoint_t ep;
    uint32_t edge = 200U + LLM_EP_FAIL_WINDOW_MS - 1U;

    memset(&ep, 0, sizeof(ep));
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 100U);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 200U);
    check_u32("decay_before", ep.consec_fail, 2U);

    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 200U + LLM_EP_FAIL_WINDOW_MS);
    check_u32("decay_reset", ep.consec_fail, 1U);
    check_state("decay_state", &ep, 200U + LLM_EP_FAIL_WINDOW_MS, LLM_EP_CLOSED);

    /* 窗口最后一毫秒仍属同一轮，第三次失败照常熔断。 */
    memset(&ep, 0, sizeof(ep));
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 100U);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 200U);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, edge);
    check_u32("decay_edge_count", ep.consec_fail, LLM_EP_FAIL_MAX);
    check_state("decay_edge_state", &ep, edge, LLM_EP_OPEN);
}

/* 指数退避：每轮探测失败重回 OPEN，保持期翻倍并封顶 OPEN_MAX_MS。 */
static void test_backoff(void)
{
    static const uint32_t want_hold[] = {LLM_EP_OPEN_MS,   2U * LLM_EP_OPEN_MS,
                                         4U * LLM_EP_OPEN_MS, 8U * LLM_EP_OPEN_MS,
                                         LLM_EP_OPEN_MAX_MS, LLM_EP_OPEN_MAX_MS};
    llm_endpoint_t ep;
    uint32_t now = 1000U;
    size_t i;

    memset(&ep, 0, sizeof(ep));
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, now);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, now);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, now);

    for (i = 0; i < sizeof(want_hold) / sizeof(want_hold[0]); i++) {
        uint32_t until = ep.open_until_ms;

        check_u32("backoff_hold", until - now, want_hold[i]);
        now = until;
        llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, now);
        llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, now);
        llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, now);
    }
}

/* 32 位取模：跨 2^32 回绕仍能正确判定到期。 */
static void test_wrap(void)
{
    llm_endpoint_t ep;
    uint32_t now = 0xFFFFFF00U;

    memset(&ep, 0, sizeof(ep));
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, now);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, now);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, now);
    check_state("wrap_open", &ep, now, LLM_EP_OPEN);
    check_state("wrap_half", &ep, now + LLM_EP_OPEN_MS, LLM_EP_HALF);

    memset(&ep, 0, sizeof(ep));
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 0xFFFF8AD0U);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 0xFFFF8AD0U);
    llm_ep_fail(&ep, AIRY_ERR_TIMEOUT, 0xFFFF8AD0U);
    check_u32("wrap_sentinel", ep.open_until_ms, 1U);
    check_state("wrap_sentinel_open", &ep, 0xFFFF8AD0U, LLM_EP_OPEN);
}

/* 落盘后重启恢复：同 provider/model 重注册即回到 OPEN，保持期按剩余量续算。 */
static void test_persist(void)
{
    llm_endpoint_t ep = make_ep("px", "mx");
    router_ctx_t *ctx;

    check_int("persist_init", llm_router_init(), 0);
    check_int("persist_register", llm_router_register_endpoint(&ep), 0);
    check_int("persist_fail", llm_rtr_fail("px", "mx", AIRY_ERR_LLM_AUTH_FAIL), 0);
    check_int("persist_admit_open", llm_rtr_admit("px", "mx"), 0);
    llm_router_destroy();

    check_int("persist_reinit", llm_router_init(), 0);
    check_int("persist_reregister", llm_router_register_endpoint(&ep), 0);
    ctx = router_ctx_get();
    check_u32("persist_restored_open", ctx->endpoints[0].open_until_ms != 0U, 1U);
    check_u32("persist_restored_fails", ctx->endpoints[0].consec_fail, 1U);
    check_u32("persist_restored_level", ctx->endpoints[0].open_level, 1U);
    check_int("persist_restored_admit", llm_rtr_admit("px", "mx"), 0);
    llm_router_destroy();
}

/* 锁内包装 + 选择路径：OPEN 端点被剔除，重注册清零健康面。 */
static void test_router(void)
{
    llm_endpoint_t a = make_ep("pa", "ma");
    llm_endpoint_t b = make_ep("pb", "mb");
    llm_route_request_t req;
    llm_route_result_t res;
    router_ctx_t *ctx;

    check_int("preinit_admit", llm_rtr_admit("pa", "ma"), 0);
    check_int("preinit_fail", llm_rtr_fail("pa", "ma", AIRY_ERR_TIMEOUT), AIRY_ERR_SYS_NOT_INIT);

    llm_router_init();

    a.cost_per_1k_input = 0.0001;
    a.cost_per_1k_output = 0.0002;
    a.consec_fail = 7U;
    a.open_until_ms = 12345U;
    b.cost_per_1k_input = 0.01;
    b.cost_per_1k_output = 0.02;
    llm_router_register_endpoint(&a);
    llm_router_register_endpoint(&b);

    ctx = router_ctx_get();
    check_u32("register_clears_fails", ctx->endpoints[0].consec_fail, 0U);
    check_u32("register_clears_open", ctx->endpoints[0].open_until_ms, 0U);

    check_int("admit_unknown", llm_rtr_admit("nope", "nope"), 0);
    check_int("fail_unknown", llm_rtr_fail("nope", "nope", AIRY_ERR_TIMEOUT), AIRY_ERR_NOT_FOUND);
    check_int("ok_unknown", llm_rtr_ok("nope", "nope"), AIRY_ERR_NOT_FOUND);
    check_int("admit_known", llm_rtr_admit("pa", "ma"), 1);

    check_int("note_auth", llm_rtr_fail("pa", "ma", AIRY_ERR_LLM_AUTH_FAIL), 0);
    check_int("admit_open", llm_rtr_admit("pa", "ma"), 0);

    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));
    req.prompt = "ping";
    req.prompt_len = 4;
    req.required_caps = LLM_CAP_CHAT | LLM_CAP_COMPLETION;
    req.strategy = LLM_ROUTE_COST;

    check_int("route_rc", llm_router_route(&req, &res), 0);
    check_str("route_skips_open", res.provider_name, "pb");

    check_int("note_ok", llm_rtr_ok("pa", "ma"), 0);
    memset(&res, 0, sizeof(res));
    check_int("route_rc2", llm_router_route(&req, &res), 0);
    check_str("route_uses_recovered", res.provider_name, "pa");

    llm_router_destroy();
}

/* 聚合健康面（0.1.20 D2-5）：三态只由端点健康状态机产生，
 * 与进程句柄存在性无关；停用端点不计入分母（与选择口径一致）。 */
static void test_aggregate(void)
{
    llm_endpoint_t a = make_ep("qa", "ma");
    llm_endpoint_t b = make_ep("qb", "mb");
    llm_endpoint_t c = make_ep("qc", "mc");
    llm_health_snap_t snap;

    c.enabled = false;

    memset(&snap, 0x5a, sizeof(snap));
    check_int("agg_preinit", (int)llm_rtr_health(&snap), (int)LLM_HEALTH_DOWN);
    check_u32("agg_preinit_total", (uint32_t)snap.total, 0U);

    llm_router_init();
    check_int("agg_empty", (int)llm_rtr_health(&snap), (int)LLM_HEALTH_DOWN);

    llm_router_register_endpoint(&a);
    llm_router_register_endpoint(&b);
    llm_router_register_endpoint(&c);
    check_int("agg_all_ok", (int)llm_rtr_health(&snap), (int)LLM_HEALTH_OK);
    check_u32("agg_skip_disabled_total", (uint32_t)snap.total, 2U);
    check_u32("agg_all_ok_ok", (uint32_t)snap.ok, 2U);

    check_u32("agg_unknown_hit", (uint32_t)llm_rtr_prov_note("nope", 0), 0U);

    check_u32("agg_hit_pa", (uint32_t)llm_rtr_prov_note("qa", 0), 1U);
    check_u32("agg_hit_pa2", (uint32_t)llm_rtr_prov_note("qa", 0), 1U);
    check_u32("agg_hit_pa3", (uint32_t)llm_rtr_prov_note("qa", 0), 1U);
    check_int("agg_degraded", (int)llm_rtr_health(&snap), (int)LLM_HEALTH_DEGRADED);
    check_u32("agg_degraded_down", (uint32_t)snap.down, 1U);

    llm_rtr_prov_note("qb", 0);
    llm_rtr_prov_note("qb", 0);
    llm_rtr_prov_note("qb", 0);
    check_int("agg_down", (int)llm_rtr_health(&snap), (int)LLM_HEALTH_DOWN);
    check_u32("agg_down_ok", (uint32_t)snap.ok, 0U);

    llm_rtr_prov_note("qa", 1);
    check_int("agg_recover_one", (int)llm_rtr_health(&snap), (int)LLM_HEALTH_DEGRADED);

    llm_rtr_prov_note("qb", 1);
    check_int("agg_recover_all", (int)llm_rtr_health(&snap), (int)LLM_HEALTH_OK);

    check_str("agg_name_ok", llm_health_name(LLM_HEALTH_OK), "ok");
    check_str("agg_name_degraded", llm_health_name(LLM_HEALTH_DEGRADED), "degraded");
    check_str("agg_name_down", llm_health_name(LLM_HEALTH_DOWN), "down");

    llm_router_destroy();
}

int main(void)
{
    printf("running endpoint health regression\n");

    snprintf(g_root, sizeof(g_root), "/tmp/airy_llm_health_%ld", (long)getpid());
    if (mkdir(g_root, 0700) != 0)
        return 1;
    /* 路径体系在首次解析时缓存：恢复用例会读写数据目录，必须先隔离。 */
    setenv("AIRY_HOME", g_root, 1);

    test_threshold();
    test_half_open();
    test_probe_outcome();
    test_auth();
    test_ignored();
    test_decay();
    test_backoff();
    test_wrap();
    test_persist();
    test_router();
    test_aggregate();

    if (fail_count == 0) {
        printf("All %d health assertions passed\n", test_count);
        return 0;
    }
    printf("%d/%d health assertions failed\n", fail_count, test_count);
    return 1;
}
