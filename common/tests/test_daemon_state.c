// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-C
/**
 * @file test_daemon_state.c
 * @brief daemon 运行面形式化状态机（daemon_state）单元测试
 *
 * G38-2「状态机穷举」前置（0.1.19 §3.7 N7 / 证伪修正 F8）：
 * 迁移全表 14 条逐条断言，外加非法状态 fail-closed 折叠与状态名呈报。
 * 四态覆盖 {正常, 降级, 不可用, 恢复}，每条迁移恰有一个测试用例。
 */

#include "daemon_state.h"

#include <stdio.h>
#include <string.h>

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_BEGIN(name)                  \
    do {                                  \
        printf("  [TEST] %s ... ", name); \
        g_tests_run++;                    \
    } while (0)

#define TEST_PASS()       \
    do {                  \
        printf("PASS\n"); \
        g_tests_passed++; \
    } while (0)

#define TEST_FAIL(msg)             \
    do {                           \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define ASSERT_TRUE(cond)     \
    do {                      \
        if (!(cond)) {        \
            TEST_FAIL(#cond); \
            return;           \
        }                     \
    } while (0)

#define ASSERT_EQ_STATE(got, want)                                     \
    do {                                                               \
        if ((got) != (want)) {                                         \
            TEST_FAIL(#got);                                           \
            printf("    got=%s want=%s\n", dmn_state_name(got),        \
                   dmn_state_name(want));                              \
            return;                                                    \
        }                                                              \
    } while (0)

/* —— 迁移全表：正常态（3 条） —— */

static void test_nominal_transitions(void)
{
    TEST_BEGIN("nominal: ok+deps stays");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_NOMINAL, true, true, false), DMN_ST_NOMINAL);
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_NOMINAL, true, true, true), DMN_ST_NOMINAL);
    TEST_PASS();

    TEST_BEGIN("nominal: dep lost degrades");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_NOMINAL, true, false, false), DMN_ST_DEGRADED);
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_NOMINAL, true, false, true), DMN_ST_DEGRADED);
    TEST_PASS();

    TEST_BEGIN("nominal: self down unavailable");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_NOMINAL, false, true, false), DMN_ST_UNAVAILABLE);
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_NOMINAL, false, false, true), DMN_ST_UNAVAILABLE);
    TEST_PASS();
}

/* —— 迁移全表：降级态（3 条） —— */

static void test_degraded_transitions(void)
{
    TEST_BEGIN("degraded: deps restored nominal");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_DEGRADED, true, true, false), DMN_ST_NOMINAL);
    TEST_PASS();

    TEST_BEGIN("degraded: dep still missing stays");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_DEGRADED, true, false, true), DMN_ST_DEGRADED);
    TEST_PASS();

    TEST_BEGIN("degraded: self down unavailable");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_DEGRADED, false, false, false), DMN_ST_UNAVAILABLE);
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_DEGRADED, false, true, true), DMN_ST_UNAVAILABLE);
    TEST_PASS();
}

/* —— 迁移全表：不可用态（4 条） —— */

static void test_unavailable_transitions(void)
{
    TEST_BEGIN("unavailable: self down stays");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_UNAVAILABLE, false, false, false),
                    DMN_ST_UNAVAILABLE);
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_UNAVAILABLE, false, true, true),
                    DMN_ST_UNAVAILABLE);
    TEST_PASS();

    TEST_BEGIN("unavailable: back with dep lost degrades");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_UNAVAILABLE, true, false, false), DMN_ST_DEGRADED);
    TEST_PASS();

    TEST_BEGIN("unavailable: back unconfirmed recovering");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_UNAVAILABLE, true, true, false), DMN_ST_RECOVERING);
    TEST_PASS();

    TEST_BEGIN("unavailable: back confirmed nominal");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_UNAVAILABLE, true, true, true), DMN_ST_NOMINAL);
    TEST_PASS();
}

/* —— 迁移全表：恢复态（4 条） —— */

static void test_recovering_transitions(void)
{
    TEST_BEGIN("recovering: probe flap falls back");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_RECOVERING, false, true, false),
                    DMN_ST_UNAVAILABLE);
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_RECOVERING, false, false, true),
                    DMN_ST_UNAVAILABLE);
    TEST_PASS();

    TEST_BEGIN("recovering: dep lost degrades");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_RECOVERING, true, false, false), DMN_ST_DEGRADED);
    TEST_PASS();

    TEST_BEGIN("recovering: window unmet stays");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_RECOVERING, true, true, false), DMN_ST_RECOVERING);
    TEST_PASS();

    TEST_BEGIN("recovering: window met nominal");
    ASSERT_EQ_STATE(dmn_state_next(DMN_ST_RECOVERING, true, true, true), DMN_ST_NOMINAL);
    TEST_PASS();
}

/* —— fail-closed 与呈报 —— */

static void test_invalid_and_names(void)
{
    TEST_BEGIN("invalid state folds fail-closed");
    ASSERT_EQ_STATE(dmn_state_next((dmn_state_t)99, true, true, true), DMN_ST_UNAVAILABLE);
    ASSERT_EQ_STATE(dmn_state_next((dmn_state_t)-1, false, false, false),
                    DMN_ST_UNAVAILABLE);
    TEST_PASS();

    TEST_BEGIN("state names for reporting");
    ASSERT_TRUE(strcmp(dmn_state_name(DMN_ST_NOMINAL), "nominal") == 0);
    ASSERT_TRUE(strcmp(dmn_state_name(DMN_ST_DEGRADED), "degraded") == 0);
    ASSERT_TRUE(strcmp(dmn_state_name(DMN_ST_UNAVAILABLE), "unavailable") == 0);
    ASSERT_TRUE(strcmp(dmn_state_name(DMN_ST_RECOVERING), "recovering") == 0);
    ASSERT_TRUE(strcmp(dmn_state_name((dmn_state_t)42), "unavailable") == 0);
    TEST_PASS();
}

/* —— 端到端迁移序列（think_d 健康面真实时序） —— */

static void test_recovery_sequence(void)
{
    dmn_state_t st = DMN_ST_NOMINAL;

    TEST_BEGIN("sequence: nominal->down->recover->nominal");
    st = dmn_state_next(st, true, true, true);
    ASSERT_EQ_STATE(st, DMN_ST_NOMINAL);
    st = dmn_state_next(st, false, false, false); /* 本体失联 */
    ASSERT_EQ_STATE(st, DMN_ST_UNAVAILABLE);
    st = dmn_state_next(st, true, true, false); /* 复达未确认 */
    ASSERT_EQ_STATE(st, DMN_ST_RECOVERING);
    st = dmn_state_next(st, true, true, true); /* 确认窗满足 */
    ASSERT_EQ_STATE(st, DMN_ST_NOMINAL);
    TEST_PASS();

    TEST_BEGIN("sequence: nominal->degrade->down->degrade->nominal");
    st = dmn_state_next(st, true, false, false); /* 依赖缺失 */
    ASSERT_EQ_STATE(st, DMN_ST_DEGRADED);
    st = dmn_state_next(st, false, false, false); /* 本体也失联 */
    ASSERT_EQ_STATE(st, DMN_ST_UNAVAILABLE);
    st = dmn_state_next(st, true, false, true); /* 复达但依赖仍缺 */
    ASSERT_EQ_STATE(st, DMN_ST_DEGRADED);
    st = dmn_state_next(st, true, true, true); /* 依赖恢复 */
    ASSERT_EQ_STATE(st, DMN_ST_NOMINAL);
    TEST_PASS();
}

int main(void)
{
    printf("daemon_state Unit Tests (G38-2 transition matrix)\n");

    test_nominal_transitions();
    test_degraded_transitions();
    test_unavailable_transitions();
    test_recovering_transitions();
    test_invalid_and_names();
    test_recovery_sequence();

    printf("\n%d/%d tests passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
