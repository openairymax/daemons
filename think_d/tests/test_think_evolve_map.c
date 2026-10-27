// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_think_evolve_map.c
 * @brief 单测：演化观测面的 JSON 归一与 counter 差分（0.1.20 W4）。
 *
 * 覆盖三条判据：
 *   1. 字段映射：learning.{patterns,detected,preemptive,effectiveness} 与
 *      顶层 records 各归位，noop 计数透传。
 *   2. 容错：缺 learning/缺字段/非数字一律归零，不产生负值或崩溃；
 *      非法 JSON 与空指针显式报错（不返回空快照）。
 *   3. 差分：单调递增取差；源重启（cur < last）回退为 cur 保证 counter
 *      单调；相等取 0。
 *
 * @note 不依赖策略库挂载：本测试只验证纯函数，与 GCCP 测试的
 * AIRY_HAS_COGNITION_STRATEGY 判据解耦。
 */

#include "service/think_evolve.h"

#include <math.h>
#include <stdio.h>

static int tests_run;
static int tests_passed;

#define TEST_PASS(name)                \
    do {                               \
        printf("  [PASS] %s\n", name); \
        tests_run++;                   \
        tests_passed++;                \
    } while (0)

#define TEST_FAIL(name, msg)                    \
    do {                                        \
        printf("  [FAIL] %s: %s\n", name, msg); \
        tests_run++;                            \
    } while (0)

#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

static void test_map_full(void)
{
    const char *json = "{\"records\":42,"
                       "\"learning\":{\"patterns\":7,\"detected\":11,"
                       "\"preemptive\":3,\"effectiveness\":0.75}}";
    think_evo_snapshot_t snap;

    if (think_evo_map(json, 5, &snap) != 0) {
        TEST_FAIL("map_full", "returned non-zero");
        return;
    }
    if (!NEAR(snap.patterns, 7) || !NEAR(snap.detections, 11) || !NEAR(snap.preemptions, 3) ||
        !NEAR(snap.effectiveness, 0.75) || !NEAR(snap.records, 42) || !NEAR(snap.evolve_noop, 5)) {
        TEST_FAIL("map_full", "field mismatch");
        return;
    }
    TEST_PASS("map_full");
}

static void test_map_missing(void)
{
    think_evo_snapshot_t snap;

    if (think_evo_map("{}", 0, &snap) != 0 || !NEAR(snap.patterns, 0) ||
        !NEAR(snap.records, 0)) {
        TEST_FAIL("map_missing", "empty object not zeroed");
        return;
    }
    if (think_evo_map("{\"learning\":{\"patterns\":\"str\"}}", 0, &snap) != 0 ||
        !NEAR(snap.patterns, 0)) {
        TEST_FAIL("map_missing", "non-number not zeroed");
        return;
    }
    TEST_PASS("map_missing");
}

static void test_map_invalid(void)
{
    think_evo_snapshot_t snap;

    if (think_evo_map("not-json", 0, &snap) == 0) {
        TEST_FAIL("map_invalid", "malformed json accepted");
        return;
    }
    if (think_evo_map(NULL, 0, &snap) == 0 || think_evo_map("{}", 0, NULL) == 0) {
        TEST_FAIL("map_invalid", "null argument accepted");
        return;
    }
    TEST_PASS("map_invalid");
}

static void test_delta(void)
{
    if (!NEAR(think_evo_delta(9, 4), 5)) {
        TEST_FAIL("delta", "monotonic growth");
        return;
    }
    if (!NEAR(think_evo_delta(4, 9), 4)) {
        TEST_FAIL("delta", "source restart");
        return;
    }
    if (!NEAR(think_evo_delta(6, 6), 0)) {
        TEST_FAIL("delta", "equal");
        return;
    }
    TEST_PASS("delta");
}

int main(void)
{
    printf("test_think_evolve_map\n");
    test_map_full();
    test_map_missing();
    test_map_invalid();
    test_delta();
    printf("  %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
