// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service_reload.c
 * @brief 0.1.20 D2-10 配置热重载（drain/世代）回归
 *
 * 锁定五条在后续重构中最易回退的口径：
 *   1. 重载成功后默认模型/提供方/价格表整体换新，世代 +1、swapped +1
 *   2. 端点表按新注册表整表重建：反复重载后端点规模等于最新配置的模型数，
 *      不随重载次数膨胀
 *   3. 配置不可读时旧配置原样继续服务，reload_fail 递增，退避窗口内二次
 *      调用直接返回 1 且不再解析
 *   4. 排空超时走强制路径：drain_timeout 记一次，旧装载进退休表，切换前
 *      取号的在途请求归还时判定世代不符（gen_mismatch）
 *   5. 无配置路径的服务不进入重载（返回 1，不产生副作用）
 */

#include "llm_service.h"
#include "router/core/llm_router.h"
#include "rpc/service.h"
#include "svc_swap.h"

#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int test_count;
static int fail_count;

static void check_int(const char *name, int got, int want)
{
    test_count++;
    if (got != want) {
        printf("    FAIL: %s: got %d, want %d\n", name, got, want);
        fail_count++;
    }
}

static void check_u32(const char *name, uint64_t got, uint64_t want)
{
    test_count++;
    if (got != want) {
        printf("    FAIL: %s: got %llu, want %llu\n", name, (unsigned long long)got,
               (unsigned long long)want);
        fail_count++;
    }
}

static void check_str(const char *name, const char *got, const char *want)
{
    test_count++;
    if (!got || strcmp(got, want) != 0) {
        printf("    FAIL: %s: got \"%s\", want \"%s\"\n", name, got ? got : "(null)", want);
        fail_count++;
    }
}

static char g_root[256];
static char g_cfg[256];

static int put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

/* 端点表规模：候选列表最多取 8，够覆盖本测试的两三个模型。 */
static int ep_count(void)
{
    llm_route_request_t req;
    llm_route_result_t out[8];
    size_t n = 0;

    __builtin_memset(&req, 0, sizeof(req));
    __builtin_memset(out, 0, sizeof(out));
    if (llm_router_pick(&req, out, 8, &n) != 0)
        return -1;
    return (int)n;
}

static void swap_stat(llm_service_t *svc, uint32_t *gen, uint32_t *swapped, uint64_t *mismatch,
                      uint64_t *tmo)
{
    svc_swap_stat(&svc->swap, gen, NULL, swapped, mismatch, tmo);
}

static const char kCfgA[] = "global:\n"
                            "  default_model: alpha\n"
                            "  default_provider: pa\n"
                            "models:\n"
                            "  - name: alpha\n"
                            "    model_id: alpha\n"
                            "    provider: pa\n"
                            "    base_url: http://127.0.0.1:9/v1\n"
                            "    input_cost_per_1k: 0.1\n"
                            "    output_cost_per_1k: 0.2\n";

/* B 有两个模型：用于验证重载后端点表与最新配置一致。 */
static const char kCfgB[] = "global:\n"
                            "  default_model: beta\n"
                            "  default_provider: pb\n"
                            "models:\n"
                            "  - name: beta\n"
                            "    model_id: beta\n"
                            "    provider: pb\n"
                            "    base_url: http://127.0.0.1:9/v1\n"
                            "    input_cost_per_1k: 0.3\n"
                            "    output_cost_per_1k: 0.4\n"
                            "  - name: beta-mini\n"
                            "    model_id: beta-mini\n"
                            "    provider: pb\n"
                            "    base_url: http://127.0.0.1:9/v1\n";

/* 无 models：providers 为空，读入段失败（模拟文件正被编辑）。 */
static const char kCfgBad[] = "global:\n"
                              "  default_model: gamma\n";

static const char kCfgD[] = "global:\n"
                            "  default_model: delta\n"
                            "  default_provider: pd\n"
                            "models:\n"
                            "  - name: delta\n"
                            "    model_id: delta\n"
                            "    provider: pd\n"
                            "    base_url: http://127.0.0.1:9/v1\n";

int main(void)
{
    printf("running config hot-reload (drain/generation) regression\n");

    snprintf(g_root, sizeof(g_root), "/tmp/airy_llm_reload_%ld", (long)getpid());
    if (mkdir(g_root, 0700) != 0)
        return 1;
    /* 路径体系在首次解析时缓存：必须在创建服务前隔离用户真实配置。 */
    setenv("AIRY_HOME", g_root, 1);
    snprintf(g_cfg, sizeof(g_cfg), "%s/model.yaml", g_root);

    if (put_file(g_cfg, kCfgA) != 0)
        return 1;

    llm_service_t *svc = llm_service_create(g_cfg);
    if (!svc) {
        printf("    FAIL: service create returned NULL\n");
        return 1;
    }

    uint32_t gen = 0;
    uint32_t swapped = 0;
    uint64_t mismatch = 0;
    uint64_t tmo = 0;

    /* 1) 创建态：Alpha 生效，世代 1，端点 1 个。 */
    check_str("create.model", svc->default_model, "alpha");
    check_u32("create.rules", (uint64_t)svc->rule_count, 1);
    swap_stat(svc, &gen, &swapped, &mismatch, &tmo);
    check_u32("create.gen", gen, 1);
    check_u32("create.swapped", swapped, 0);
    check_int("create.ep", ep_count(), 1);

    /* 2) A → B：换默认模型、换注册表、端点表跟随，世代 +1。 */
    if (put_file(g_cfg, kCfgB) != 0)
        return 1;
    check_int("swap.rc", llm_service_reload(svc), AIRY_OK);
    check_str("swap.model", svc->default_model, "beta");
    check_str("swap.provider", svc->default_provider, "pb");
    check_u32("swap.rules", (uint64_t)svc->rule_count, 1);
    check_u32("swap.reload_ok", svc->reload_ok, 1);
    swap_stat(svc, &gen, &swapped, &mismatch, &tmo);
    check_u32("swap.gen", gen, 2);
    check_u32("swap.swapped", swapped, 1);
    check_int("swap.ep", ep_count(), 2);
    /* 价格表必须跟着换：beta 的输入价 0.3 落在新规则里。 */
    {
        double in = -1.0;
        double out = -1.0;
        cost_tracker_price((const pricing_rule_t *)svc->rules, (int)svc->rule_count, "beta", &in,
                           &out);
        check_int("swap.price_in", (int)(in * 10.0 + 0.5), 3);
    }

    /* 3) 反复重载同一份配置：端点表不膨胀，世代逐次 +1。 */
    check_int("repeat.rc1", llm_service_reload(svc), AIRY_OK);
    check_int("repeat.rc2", llm_service_reload(svc), AIRY_OK);
    swap_stat(svc, &gen, &swapped, &mismatch, &tmo);
    check_u32("repeat.gen", gen, 4);
    check_u32("repeat.reload_ok", svc->reload_ok, 3);
    check_int("repeat.ep", ep_count(), 2);

    /* 4) 排空超时：切换前取号的请求被世代裁决丢弃，旧装载进退休表。 */
    if (put_file(g_cfg, kCfgD) != 0)
        return 1;
    svc->swap.drain_ms = 50U;
    uint32_t inflight_gen = 0;
    check_int("force.enter", svc_swap_enter(&svc->swap, &inflight_gen), 0);
    check_int("force.rc", llm_service_reload(svc), AIRY_OK);
    check_str("force.model", svc->default_model, "delta");
    swap_stat(svc, &gen, &swapped, &mismatch, &tmo);
    check_u32("force.gen", gen, 5);
    check_u32("force.drain_tmo", tmo, 1);
    check_u32("force.mismatch_pre", mismatch, 0);
    check_int("force.retired", svc->retired_n, 1);
    check_int("force.leave_stale", svc_swap_leave(&svc->swap, inflight_gen), 0);
    swap_stat(svc, &gen, &swapped, &mismatch, &tmo);
    check_u32("force.mismatch", mismatch, 1);
    check_u32("force.inflight", (uint64_t)svc->swap.inflight, 0);

    /* 5) 读入失败：旧配置原样服务，退避窗口内二次调用不再解析。 */
    if (put_file(g_cfg, kCfgBad) != 0)
        return 1;
    check_int("bad.rc", llm_service_reload(svc), AIRY_ERR_STATE_ERROR);
    check_u32("bad.fail", svc->reload_fail, 1);
    check_str("bad.model_kept", svc->default_model, "delta");
    swap_stat(svc, &gen, &swapped, &mismatch, &tmo);
    check_u32("bad.gen_kept", gen, 5);
    check_int("bad.backoff_rc", llm_service_reload(svc), 1);
    check_u32("bad.backoff_fail", svc->reload_fail, 1);

    llm_service_destroy(svc);

    /* 6) 无配置路径的服务：重载直接返回 1，不产生副作用。 */
    llm_service_t *bare = llm_service_create(NULL);
    if (!bare) {
        printf("    FAIL: bare service create returned NULL\n");
        return 1;
    }
    check_int("bare.rc", llm_service_reload(bare), 1);
    check_u32("bare.fail", bare->reload_fail, 0);
    llm_service_destroy(bare);

    if (fail_count == 0) {
        printf("All %d reload assertions passed\n", test_count);
        return 0;
    }
    printf("%d/%d reload assertions failed\n", fail_count, test_count);
    return 1;
}
