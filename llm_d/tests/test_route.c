// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_route.c
 * @brief 0.1.19 §3.8.4 A4 model.yaml routing 段解析回归
 *
 * 锁定五条容易在后续重构中回退的口径：
 *   1. 段缺失/文件不可读不改变既有配置，缺省仍是 off
 *   2. mode 仅 auto 开启切换，未知取值告警后按 off
 *   3. chain_depth 限幅 [0, SVC_CHAIN_MAX_TRIED]，负值按 0
 *   4. on 序列逐项映射白名单位；缺失保留缺省，显式空表为 0
 *   5. deadline_sec 按秒 ×1000 落入 budget_ms，<=0 不限，防 int 溢出
 *
 * 另验「主配置 + 用户覆盖」叠加：后调用方只覆盖自己显式声明的键。
 */

#include "rpc/service_chain.h"

#include "error_codes.h"
#include "router/core/llm_failover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void check_u32(const char *name, unsigned got, unsigned want)
{
    test_count++;
    if (got != want) {
        printf("    FAIL: %s: got 0x%x, want 0x%x\n", name, got, want);
        fail_count++;
    }
}

static const char *tmp_base(void)
{
#if defined(_WIN32)
    const char *t = getenv("TEMP");
    return (t && t[0]) ? t : ".";
#else
    const char *t = getenv("TMPDIR");
    return (t && t[0]) ? t : "/tmp";
#endif
}

static int write_cfg(const char *name, const char *text)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", tmp_base(), name);
    f = fopen(path, "wb");
    if (!f)
        return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

static void cfg_path(const char *name, char *out, size_t sz)
{
    snprintf(out, sz, "%s/%s", tmp_base(), name);
}

/* 一份无 routing 段的合法配置：用于验证「缺失不覆盖」。 */
static void test_absent(void)
{
    llm_route_cfg_t cfg;
    char path[512];

    if (write_cfg("airy-route-absent.yaml", "models:\n  - name: A\n    model_id: a\n") != 0)
        return;
    cfg_path("airy-route-absent.yaml", path, sizeof(path));

    svc_chain_def(&cfg);
    check_int("absent.rc", svc_route_load(path, &cfg), AIRY_ERR_NOT_FOUND);
    check_int("absent.mode", cfg.mode, SVC_CHAIN_OFF);
    check_int("absent.depth", cfg.depth, SVC_CHAIN_DEF_DEPTH);
    check_u32("absent.on", cfg.on_mask, LLM_FO_ON_DEFAULT);
    check_int("absent.budget", cfg.budget_ms, 0);
}

static void test_args(void)
{
    llm_route_cfg_t cfg;
    char path[512];

    svc_chain_def(&cfg);
    check_int("args.null_path", svc_route_load(NULL, &cfg), AIRY_ERR_INVALID_PARAM);
    check_int("args.null_cfg",
              svc_route_load("nonexistent-route.yaml", NULL), AIRY_ERR_INVALID_PARAM);
    check_int("args.no_file", svc_route_load("/nonexistent-airy/model.yaml", &cfg),
              AIRY_ERR_IO);
    cfg_path("airy-route-absent.yaml", path, sizeof(path));
    check_int("args.mode_kept", cfg.mode, SVC_CHAIN_OFF);
}

static void test_mode(void)
{
    llm_route_cfg_t cfg;
    char path[512];

    if (write_cfg("airy-route-mode.yaml", "routing:\n  mode: auto\n") != 0)
        return;
    cfg_path("airy-route-mode.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    check_int("mode.auto_rc", svc_route_load(path, &cfg), AIRY_OK);
    check_int("mode.auto", cfg.mode, SVC_CHAIN_AUTO);
    check_int("mode.auto_depth_kept", cfg.depth, SVC_CHAIN_DEF_DEPTH);

    if (write_cfg("airy-route-mode2.yaml", "routing:\n  mode: AUTO\n") != 0)
        return;
    cfg_path("airy-route-mode2.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    check_int("mode.case_rc", svc_route_load(path, &cfg), AIRY_OK);
    check_int("mode.case", cfg.mode, SVC_CHAIN_AUTO);

    if (write_cfg("airy-route-mode3.yaml", "routing:\n  mode: weird\n") != 0)
        return;
    cfg_path("airy-route-mode3.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    cfg.mode = SVC_CHAIN_AUTO;
    check_int("mode.unknown_rc", svc_route_load(path, &cfg), AIRY_OK);
    check_int("mode.unknown_off", cfg.mode, SVC_CHAIN_OFF);
}

static void test_depth(void)
{
    llm_route_cfg_t cfg;
    char path[512];

    if (write_cfg("airy-route-depth.yaml", "routing:\n  chain_depth: 3\n") != 0)
        return;
    cfg_path("airy-route-depth.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_int("depth.three", cfg.depth, 3);

    if (write_cfg("airy-route-depth2.yaml", "routing:\n  chain_depth: 0\n") != 0)
        return;
    cfg_path("airy-route-depth2.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_int("depth.zero", cfg.depth, 0);

    if (write_cfg("airy-route-depth3.yaml", "routing:\n  chain_depth: 99\n") != 0)
        return;
    cfg_path("airy-route-depth3.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_int("depth.clamp_high", cfg.depth, SVC_CHAIN_MAX_TRIED);

    if (write_cfg("airy-route-depth4.yaml", "routing:\n  chain_depth: -4\n") != 0)
        return;
    cfg_path("airy-route-depth4.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_int("depth.negative", cfg.depth, 0);

    if (write_cfg("airy-route-depth5.yaml", "routing:\n  chain_depth: lots\n") != 0)
        return;
    cfg_path("airy-route-depth5.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_int("depth.nonnumeric", cfg.depth, 0);
}

static void test_on(void)
{
    llm_route_cfg_t cfg;
    char path[512];

    if (write_cfg("airy-route-on.yaml",
                  "routing:\n  on: [transport]\n") != 0)
        return;
    cfg_path("airy-route-on.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_u32("on.transport", cfg.on_mask, LLM_FO_BIT(LLM_FO_TRANSPORT));

    if (write_cfg("airy-route-on2.yaml",
                  "routing:\n  on:\n    - rate_limit\n    - auth\n") != 0)
        return;
    cfg_path("airy-route-on2.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_u32("on.two",
              cfg.on_mask,
              (unsigned)(LLM_FO_BIT(LLM_FO_RATE_LIMIT) | LLM_FO_BIT(LLM_FO_AUTH)));

    if (write_cfg("airy-route-on3.yaml", "routing:\n  on: []\n") != 0)
        return;
    cfg_path("airy-route-on3.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_u32("on.empty", cfg.on_mask, 0);

    if (write_cfg("airy-route-on4.yaml",
                  "routing:\n  on: [auth, bogus]\n") != 0)
        return;
    cfg_path("airy-route-on4.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_u32("on.unknown_skipped", cfg.on_mask, LLM_FO_BIT(LLM_FO_AUTH));

    if (write_cfg("airy-route-on5.yaml", "routing:\n  on: transport\n") != 0)
        return;
    cfg_path("airy-route-on5.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_u32("on.not_seq_kept", cfg.on_mask, LLM_FO_ON_DEFAULT);
}

static void test_deadline(void)
{
    llm_route_cfg_t cfg;
    char path[512];

    if (write_cfg("airy-route-dl.yaml", "routing:\n  deadline_sec: 5\n") != 0)
        return;
    cfg_path("airy-route-dl.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_int("deadline.5s", cfg.budget_ms, 5000);

    if (write_cfg("airy-route-dl2.yaml", "routing:\n  deadline_sec: 0\n") != 0)
        return;
    cfg_path("airy-route-dl2.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    cfg.budget_ms = 7000;
    svc_route_load(path, &cfg);
    check_int("deadline.zero", cfg.budget_ms, 0);

    if (write_cfg("airy-route-dl3.yaml", "routing:\n  deadline_sec: 9999999999\n") != 0)
        return;
    cfg_path("airy-route-dl3.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    svc_route_load(path, &cfg);
    check_int("deadline.clamp", cfg.budget_ms > 0, 1);
    check_int("deadline.clamp_exact", cfg.budget_ms, 2147483000);
}

static void test_bad_section(void)
{
    llm_route_cfg_t cfg;
    char path[512];

    if (write_cfg("airy-route-bad.yaml", "routing: auto\n") != 0)
        return;
    cfg_path("airy-route-bad.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    check_int("bad.rc", svc_route_load(path, &cfg), AIRY_ERR_INVALID_PARAM);
    check_int("bad.mode", cfg.mode, SVC_CHAIN_OFF);
}

/* 主配置被用户覆盖：用户只写 chain_depth，其余键保持主配置取值。 */
static void test_layering(void)
{
    llm_route_cfg_t cfg;
    char path[512];

    if (write_cfg("airy-route-main.yaml",
                  "routing:\n  mode: auto\n  chain_depth: 1\n  deadline_sec: 9\n") != 0)
        return;
    cfg_path("airy-route-main.yaml", path, sizeof(path));
    svc_chain_def(&cfg);
    check_int("layer.main_rc", svc_route_load(path, &cfg), AIRY_OK);
    check_int("layer.main_mode", cfg.mode, SVC_CHAIN_AUTO);
    check_int("layer.main_depth", cfg.depth, 1);
    check_int("layer.main_budget", cfg.budget_ms, 9000);

    if (write_cfg("airy-route-user.yaml", "routing:\n  chain_depth: 3\n") != 0)
        return;
    cfg_path("airy-route-user.yaml", path, sizeof(path));
    check_int("layer.user_rc", svc_route_load(path, &cfg), AIRY_OK);
    check_int("layer.user_mode_kept", cfg.mode, SVC_CHAIN_AUTO);
    check_int("layer.user_depth", cfg.depth, 3);
    check_int("layer.user_budget_kept", cfg.budget_ms, 9000);
    check_u32("layer.user_on_kept", cfg.on_mask, LLM_FO_ON_DEFAULT);
}

int main(void)
{
    printf("running routing-section parse regression\n");

    test_absent();
    test_args();
    test_mode();
    test_depth();
    test_on();
    test_deadline();
    test_bad_section();
    test_layering();

    if (fail_count == 0) {
        printf("All %d route assertions passed\n", test_count);
        return 0;
    }
    printf("%d/%d route assertions failed\n", fail_count, test_count);
    return 1;
}
