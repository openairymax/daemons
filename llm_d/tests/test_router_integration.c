// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_router_integration.c
 * @brief P3.16 (ACC-DT17): llm_router 集成测试
 *
 * 验证 ACC-DT17 验收标准：llm_router 被集成且路由策略生效。
 *
 * 测试内容：
 *   1. COST 策略选择成本最低的端点
 *   2. ROUND_ROBIN 策略在端点间轮询（负载均衡）
 *   3. service 生命周期集成 — create 初始化 router、destroy 销毁 router、
 *      router 在 service 生命周期内可用且 total_requests 递增
 *   4. 测量面（0.1.20 D0）— llm_rtr_meas 的端点 EWMA 回填、lat/timings
 *      聚合口径、缓存命中回放（provider NULL）只并 token 不记延迟
 *
 * 每个测试为独立可执行进程内的函数，通过 init/destroy 保证全局单例隔离。
 *
 * @note 不使用 assert() 执行副作用操作：Release 构建类型定义 NDEBUG，会将
 *       assert(expr) 展开为 ((void)0)，导致 expr 中的函数调用（init/register/
 *       get_stats）根本不执行。所有副作用操作必须用显式 if 检查 + TEST_FAIL。
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "llm_service.h"
#include "router/core/llm_router.h"
#include "router/core/router_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_count = 0;
static int pass_count = 0;

#define TEST_PASS()   \
    do {              \
        pass_count++; \
        test_count++; \
    } while (0)
#define TEST_FAIL(msg)                 \
    do {                               \
        printf("    FAIL: %s\n", msg); \
        test_count++;                  \
    } while (0)

static llm_endpoint_t make_endpoint(const char *provider, const char *model, double cost_in,
                                    double cost_out, uint32_t latency_ms, uint32_t caps)
{
    llm_endpoint_t ep;
    memset(&ep, 0, sizeof(ep));
    snprintf(ep.provider_name, sizeof(ep.provider_name), "%s", provider);
    snprintf(ep.model_name, sizeof(ep.model_name), "%s", model);
    ep.enabled = true;
    ep.priority = 0;
    ep.context_window = 8192;
    ep.cost_per_1k_input = cost_in;
    ep.cost_per_1k_output = cost_out;
    ep.avg_latency_ms = latency_ms;
    ep.capabilities = caps;
    return ep;
}

static llm_route_request_t make_request(llm_route_strategy_t strategy, uint32_t required_caps,
                                        const char *prompt, uint32_t max_tokens)
{
    llm_route_request_t req;
    memset(&req, 0, sizeof(req));
    req.prompt = prompt ? prompt : "";
    req.prompt_len = prompt ? strlen(prompt) : 0;
    req.required_caps = required_caps;
    req.max_tokens = max_tokens;
    req.max_cost = 0;
    req.max_latency_ms = 0;
    req.strategy = strategy;
    req.preferred_provider[0] = '\0';
    return req;
}

static void test_cost_strategy_selects_cheapest(void)
{
    printf("  [ACC-DT17.1] COST strategy selects cheapest endpoint...\n");

    if (llm_router_init() != 0) {
        TEST_FAIL("llm_router_init failed");
        return;
    }

    /* Register 3 endpoints with the same capabilities but decreasing cost:
     *   gpt-4o     : 0.030   / 0.060    (most expensive)
     *   gpt-3.5    : 0.001   / 0.002    (medium)
     *   deepseek   : 0.00014 / 0.00028  (cheapest) */
    const uint32_t caps = LLM_CAP_CHAT | LLM_CAP_COMPLETION | LLM_CAP_STREAMING;
    llm_endpoint_t e1 = make_endpoint("openai", "gpt-4o", 0.030, 0.060, 1200, caps);
    llm_endpoint_t e2 = make_endpoint("openai", "gpt-3.5", 0.001, 0.002, 1000, caps);
    llm_endpoint_t e3 = make_endpoint("deepseek", "deepseek-v3", 0.00014, 0.00028, 900, caps);
    if (llm_router_register_endpoint(&e1) != 0 || llm_router_register_endpoint(&e2) != 0 ||
        llm_router_register_endpoint(&e3) != 0) {
        TEST_FAIL("register_endpoint failed");
        llm_router_destroy();
        return;
    }

    llm_router_stats_t pre_stats;
    if (llm_router_get_stats(&pre_stats) != 0) {
        TEST_FAIL("llm_router_get_stats failed (pre-route)");
        llm_router_destroy();
        return;
    }
    if (pre_stats.total_requests != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected pre-route total_requests==0, got %llu",
                 (unsigned long long)pre_stats.total_requests);
        TEST_FAIL(msg);
        llm_router_destroy();
        return;
    }

    llm_route_request_t req = make_request(LLM_ROUTE_COST, LLM_CAP_CHAT | LLM_CAP_COMPLETION,
                                           "hello world this is a routing test", 100);
    llm_route_result_t result;
    memset(&result, 0, sizeof(result));

    int rc = llm_router_route(&req, &result);
    if (rc != 0) {
        TEST_FAIL("COST route returned non-zero");
        llm_router_destroy();
        return;
    }

    printf("    COST routed -> %s/%s (cost=$%.6f, strategy=%d)\n", result.provider_name,
           result.model_name, result.estimated_cost, (int)result.strategy_used);

    if (strcmp(result.model_name, "deepseek-v3") != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected deepseek-v3, got %s", result.model_name);
        TEST_FAIL(msg);
        llm_router_destroy();
        return;
    }

    llm_router_stats_t stats;
    if (llm_router_get_stats(&stats) != 0) {
        TEST_FAIL("llm_router_get_stats failed (post-route)");
        llm_router_destroy();
        return;
    }
    if (stats.total_requests < 1) {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected post-route total_requests>=1, got %llu",
                 (unsigned long long)stats.total_requests);
        TEST_FAIL(msg);
        llm_router_destroy();
        return;
    }

    TEST_PASS();
    printf("    PASSED (cheapest endpoint selected, total_requests=%llu)\n",
           (unsigned long long)stats.total_requests);

    llm_router_destroy();
}

static void test_round_robin_cycles(void)
{
    printf("  [ACC-DT17.2] ROUND_ROBIN cycles through endpoints...\n");

    if (llm_router_init() != 0) {
        TEST_FAIL("llm_router_init failed");
        return;
    }

    const uint32_t caps = LLM_CAP_CHAT | LLM_CAP_COMPLETION;
    llm_endpoint_t ea = make_endpoint("openai", "model-a", 0.001, 0.002, 1000, caps);
    llm_endpoint_t eb = make_endpoint("anthropic", "model-b", 0.002, 0.004, 1100, caps);
    if (llm_router_register_endpoint(&ea) != 0 || llm_router_register_endpoint(&eb) != 0) {
        TEST_FAIL("register_endpoint failed");
        llm_router_destroy();
        return;
    }

    llm_router_stats_t pre_stats;
    if (llm_router_get_stats(&pre_stats) != 0) {
        TEST_FAIL("llm_router_get_stats failed (pre-route)");
        llm_router_destroy();
        return;
    }
    if (pre_stats.total_requests != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected pre-route total_requests==0, got %llu",
                 (unsigned long long)pre_stats.total_requests);
        TEST_FAIL(msg);
        llm_router_destroy();
        return;
    }

    llm_route_request_t req = make_request(LLM_ROUTE_ROUND_ROBIN, caps, "rotate", 50);

    char models[4][64];
    for (int i = 0; i < 4; i++) {
        llm_route_result_t result;
        memset(&result, 0, sizeof(result));
        int rc = llm_router_route(&req, &result);
        if (rc != 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "ROUND_ROBIN route %d returned non-zero", i);
            TEST_FAIL(msg);
            llm_router_destroy();
            return;
        }
        snprintf(models[i], sizeof(models[i]), "%s", result.model_name);
        printf("    round %d -> %s/%s\n", i, result.provider_name, result.model_name);
    }

    if (strcmp(models[0], models[2]) == 0 && strcmp(models[1], models[3]) == 0 &&
        strcmp(models[0], models[1]) != 0) {
        TEST_PASS();
        printf("    PASSED (alternated %s <-> %s)\n", models[0], models[1]);
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "expected alternation, got %s,%s,%s,%s", models[0], models[1],
                 models[2], models[3]);
        TEST_FAIL(msg);
    }

    llm_router_destroy();
}

static void test_service_lifecycle_integration(void)
{
    printf("  [ACC-DT17.3] service lifecycle integrates router...\n");

    llm_service_t *svc = llm_service_create(NULL);
    if (svc == NULL) {
        TEST_FAIL("llm_service_create returned NULL");
        return;
    }

    /* After create, the router should already be initialized and stats
     * accessible with total_requests==0 (empty registry ->
     * register_router_endpoints registers 0 endpoints) */
    llm_router_stats_t stats;
    if (llm_router_get_stats(&stats) != 0) {
        TEST_FAIL("llm_router_get_stats failed after service create");
        llm_service_destroy(svc);
        return;
    }
    if (stats.total_requests != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected total_requests==0 after create, got %llu",
                 (unsigned long long)stats.total_requests);
        TEST_FAIL(msg);
        llm_service_destroy(svc);
        return;
    }
    printf("    after create: router stats accessible, total_requests=0\n");

    const uint32_t caps = LLM_CAP_CHAT | LLM_CAP_COMPLETION;
    llm_endpoint_t ep = make_endpoint("openai", "gpt-4o", 0.03, 0.06, 1200, caps);
    if (llm_router_register_endpoint(&ep) != 0) {
        TEST_FAIL("register_endpoint failed within service lifecycle");
        llm_service_destroy(svc);
        return;
    }

    llm_route_request_t req = make_request(LLM_ROUTE_COST, caps, "integration probe", 100);
    llm_route_result_t result;
    memset(&result, 0, sizeof(result));
    int rc = llm_router_route(&req, &result);
    if (rc != 0) {
        TEST_FAIL("router_route failed within service lifecycle");
        llm_service_destroy(svc);
        return;
    }

    if (llm_router_get_stats(&stats) != 0) {
        TEST_FAIL("llm_router_get_stats failed after route");
        llm_service_destroy(svc);
        return;
    }
    if (stats.total_requests >= 1) {
        printf("    after route: total_requests=%llu (incremented)\n",
               (unsigned long long)stats.total_requests);
        TEST_PASS();
        printf("    PASSED (router live within service lifecycle)\n");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "total_requests did not increment (got %llu)",
                 (unsigned long long)stats.total_requests);
        TEST_FAIL(msg);
    }

    llm_service_destroy(svc);
}

/* RPC stats 导出面（D0/G47）：router 与 measurement 两对象及分列键必须可见。 */
static void test_stats_export_surface(void)
{
    printf("  [D0.1] llm_service_stats exports router/measurement...\n");

    llm_service_t *svc = llm_service_create(NULL);
    if (svc == NULL) {
        TEST_FAIL("llm_service_create returned NULL");
        return;
    }

    char *json = NULL;
    if (llm_service_stats(svc, &json) != 0 || json == NULL) {
        TEST_FAIL("llm_service_stats failed");
        llm_service_destroy(svc);
        return;
    }

    static const char *const keys[] = {"\"router\"",
                                       "\"measurement\"",
                                       "\"avg_latency_ms\"",
                                       "\"meas_samples\"",
                                       "\"prefill_tokens\"",
                                       "\"decode_tokens\"",
                                       "\"cached_tokens\""};
    int missing = 0;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strstr(json, keys[i]) == NULL) {
            printf("    missing key: %s\n", keys[i]);
            missing++;
        }
    }
    free(json);

    llm_service_destroy(svc);
    if (missing == 0) {
        TEST_PASS();
        printf("    PASSED (export surface complete)\n");
    } else {
        TEST_FAIL("stats export missing keys");
    }
}

static void expect_u64(const char *what, uint64_t got, uint64_t want)
{
    if (got == want)
        return;
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: got %llu, want %llu", what, (unsigned long long)got,
             (unsigned long long)want);
    TEST_FAIL(msg);
}

static void expect_dbl(const char *what, double got, double want)
{
    if (got == want)
        return;
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: got %g, want %g", what, got, want);
    TEST_FAIL(msg);
}

/* 测量面单点记账：EWMA(α=0.2) 回填、timings 零值守卫、命中回放、未注册端点。 */
static void test_measurement_backfill(void)
{
    printf("  [D0.2] llm_rtr_meas EWMA backfill + stats aggregation...\n");

    llm_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.prompt_tokens = 100;
    resp.cached_tokens = 40;

    if (llm_rtr_meas("llama", "qwen", &resp, 500) == 0) {
        TEST_FAIL("meas before init should fail");
        return;
    }
    if (llm_router_init() != 0) {
        TEST_FAIL("llm_router_init failed");
        return;
    }
    if (llm_rtr_meas("llama", "qwen", NULL, 500) == 0) {
        TEST_FAIL("meas with NULL resp should fail");
        llm_router_destroy();
        return;
    }

    const uint32_t caps = LLM_CAP_CHAT | LLM_CAP_COMPLETION;
    llm_endpoint_t ep = make_endpoint("llama", "qwen", 0.001, 0.002, 1000, caps);
    if (llm_router_register_endpoint(&ep) != 0) {
        TEST_FAIL("register_endpoint failed");
        llm_router_destroy();
        return;
    }

    /* 种子 1000 + 实测 500：(1000*4+500+2)/5 = 900。 */
    (void)llm_rtr_meas("llama", "qwen", &resp, 500);

    /* 带 timings 的 600：(900*4+600+2)/5 = 840。 */
    resp.timings.prefill_tokens = 100;
    resp.timings.decode_tokens = 20;
    resp.timings.kv_cache_tokens = 40;
    resp.timings.prefill_ms = 12.5;
    resp.timings.decode_ms = 88.0;
    (void)llm_rtr_meas("llama", "qwen", &resp, 600);

    /* 命中回放（provider NULL）：只并 token，不动 lat 与 meas 样本。 */
    (void)llm_rtr_meas(NULL, NULL, &resp, 0);

    /* 未注册端点：EWMA 无对象，但请求级统计不丢。 */
    (void)llm_rtr_meas("ghost", "x", &resp, 300);

    router_ctx_t *ctx = router_ctx_get();
    if (ctx->endpoints[0].avg_latency_ms != 840U) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ep avg: got %u, want 840", ctx->endpoints[0].avg_latency_ms);
        TEST_FAIL(msg);
    }

    llm_router_stats_t s;
    if (llm_router_get_stats(&s) != 0) {
        TEST_FAIL("llm_router_get_stats failed");
        llm_router_destroy();
        return;
    }
    expect_u64("lat_samples", s.lat_samples, 3);
    expect_u64("lat_total_ms", s.lat_total_ms, 1400);
    expect_u64("prompt_tokens_sum", s.prompt_tokens_sum, 400);
    expect_u64("cached_tokens_sum", s.cached_tokens_sum, 160);
    expect_u64("meas_samples", s.meas_samples, 2);
    expect_u64("prefill_tokens", s.prefill_tokens, 200);
    expect_u64("decode_tokens", s.decode_tokens, 40);
    expect_u64("kv_cache_tokens", s.kv_cache_tokens, 80);
    expect_dbl("prefill_ms", s.prefill_ms, 25.0);
    expect_dbl("decode_ms", s.decode_ms, 176.0);

    TEST_PASS();
    printf("    PASSED (EWMA 840, lat/meas aggregation live)\n");
    llm_router_destroy();
}

int main(void)
{
    char root[128];

    snprintf(root, sizeof(root), "/tmp/airy_llm_rtr_%ld", (long)getpid());
    if (mkdir(root, 0700) != 0)
        return 1;
    /* 健康面随 router 生命周期落盘/恢复，用例必须隔离到临时数据目录。 */
    setenv("AIRY_HOME", root, 1);

    printf("=========================================\n");
    printf("  P3.16 (ACC-DT17): llm_router Integration Tests\n");
    printf("=========================================\n\n");

    test_cost_strategy_selects_cheapest();
    test_round_robin_cycles();
    test_service_lifecycle_integration();
    test_stats_export_surface();
    test_measurement_backfill();

    printf("\n=========================================\n");
    printf("  Results: %d/%d tests PASSED\n", pass_count, test_count);
    printf("=========================================\n");

    return (pass_count == test_count) ? 0 : 1;
}
