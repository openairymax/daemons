// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-C
/**
 * @file test_svc_swap.c
 * @brief 配置热切换排空闸门 / 世代裁决（svc_swap）单元测试
 *
 * 覆盖 0.1.20 D2-10 的三条判定契约：
 * - 排空中新请求被拒（AIRY_ERR_BUSY），排空完成后恢复接客且世代 +1
 * - 排空按在途请求数判定：有在途时 begin 阻塞等待，归还后正常完成
 * - 排空超时返回强制切换并计 drain_timeout；旧世代请求归还时结果被
 *   丢弃并计 gen_mismatch（绝不混用两代结果）
 * - abort 撤销 draining 且世代不变（旧配置继续服务）
 * - 未 begin 即 commit 属状态错误，不得推进世代
 */

#include "svc_swap.h"

#include <stdio.h>

#include "error.h"
#include "platform_misc.h"
#include "platform_process.h"

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

#define ASSERT_EQ_INT(a, b)          \
    do {                             \
        if ((a) != (b)) {            \
            TEST_FAIL(#a " != " #b); \
            return;                  \
        }                            \
    } while (0)

static void test_init_guards(void)
{
    svc_swap_t s;

    TEST_BEGIN("init_guards");
    ASSERT_EQ_INT(svc_swap_init(NULL, 0), AIRY_ERR_INVALID_PARAM);
    ASSERT_EQ_INT(svc_swap_init(&s, 0), 0);
    ASSERT_EQ_INT(s.drain_ms, SVC_SWAP_DRAIN_MS);
    ASSERT_EQ_INT(s.gen, 1);
    svc_swap_done(&s);
    ASSERT_EQ_INT(svc_swap_init(&s, 50), 0);
    ASSERT_EQ_INT(s.drain_ms, 50);
    svc_swap_done(&s);
    TEST_PASS();
}

static void test_enter_leave(void)
{
    svc_swap_t s;
    uint32_t gen = 0;
    uint32_t inflight = 99;
    uint64_t mismatch = 99;

    TEST_BEGIN("enter_leave");
    ASSERT_EQ_INT(svc_swap_init(&s, 0), 0);
    ASSERT_EQ_INT(svc_swap_enter(NULL, &gen), AIRY_ERR_INVALID_PARAM);
    ASSERT_EQ_INT(svc_swap_enter(&s, NULL), AIRY_ERR_INVALID_PARAM);
    ASSERT_EQ_INT(svc_swap_enter(&s, &gen), 0);
    ASSERT_EQ_INT(gen, 1);

    svc_swap_stat(&s, NULL, &inflight, NULL, &mismatch, NULL);
    ASSERT_EQ_INT(inflight, 1);
    ASSERT_EQ_INT(mismatch, 0);

    ASSERT_EQ_INT(svc_swap_leave(&s, gen), 1);
    svc_swap_stat(&s, NULL, &inflight, NULL, &mismatch, NULL);
    ASSERT_EQ_INT(inflight, 0);
    ASSERT_EQ_INT(mismatch, 0);
    svc_swap_done(&s);
    TEST_PASS();
}

static void test_busy_while_draining(void)
{
    svc_swap_t s;
    uint32_t gen = 0;
    uint32_t swapped = 0;

    TEST_BEGIN("busy_while_draining");
    ASSERT_EQ_INT(svc_swap_init(&s, 0), 0);
    ASSERT_EQ_INT(svc_swap_begin(&s), 0);
    ASSERT_EQ_INT(svc_swap_enter(&s, &gen), AIRY_ERR_BUSY);
    /* 他人正在切换：第二次 begin 拒绝，不得抢占同一 draining 窗口 */
    ASSERT_EQ_INT(svc_swap_begin(&s), AIRY_ERR_BUSY);

    ASSERT_EQ_INT(svc_swap_commit(&s), 0);
    svc_swap_stat(&s, NULL, NULL, &swapped, NULL, NULL);
    ASSERT_EQ_INT(swapped, 1);

    ASSERT_EQ_INT(svc_swap_enter(&s, &gen), 0);
    ASSERT_EQ_INT(gen, 2);
    ASSERT_EQ_INT(svc_swap_leave(&s, gen), 1);
    svc_swap_done(&s);
    TEST_PASS();
}

static void test_abort_keeps_gen(void)
{
    svc_swap_t s;
    uint32_t gen = 0;

    TEST_BEGIN("abort_keeps_gen");
    ASSERT_EQ_INT(svc_swap_init(&s, 0), 0);
    ASSERT_EQ_INT(svc_swap_begin(&s), 0);
    svc_swap_abort(&s);
    ASSERT_EQ_INT(svc_swap_enter(&s, &gen), 0);
    ASSERT_EQ_INT(gen, 1);
    ASSERT_EQ_INT(svc_swap_leave(&s, gen), 1);
    svc_swap_done(&s);
    TEST_PASS();
}

static void test_commit_without_begin(void)
{
    svc_swap_t s;
    uint32_t gen = 0;

    TEST_BEGIN("commit_without_begin");
    ASSERT_EQ_INT(svc_swap_init(&s, 0), 0);
    ASSERT_EQ_INT(svc_swap_commit(&s), AIRY_ERR_STATE_ERROR);
    ASSERT_EQ_INT(svc_swap_enter(&s, &gen), 0);
    ASSERT_EQ_INT(gen, 1);
    ASSERT_EQ_INT(svc_swap_leave(&s, gen), 1);
    svc_swap_done(&s);
    TEST_PASS();
}

static void test_drain_timeout_forced(void)
{
    svc_swap_t s;
    uint32_t gen = 0;
    uint32_t fresh = 0;
    uint64_t mismatch = 0;
    uint64_t tmo = 0;

    TEST_BEGIN("drain_timeout_forced");
    ASSERT_EQ_INT(svc_swap_init(&s, 20), 0);
    ASSERT_EQ_INT(svc_swap_enter(&s, &gen), 0);
    ASSERT_EQ_INT(gen, 1);

    /* 20ms 内不归还 → 强制切换，旧配置让位 */
    ASSERT_EQ_INT(svc_swap_begin(&s), 1);
    ASSERT_EQ_INT(svc_swap_commit(&s), 0);

    /* 旧世代在途请求归还：结果必须被丢弃 */
    ASSERT_EQ_INT(svc_swap_leave(&s, gen), 0);
    svc_swap_stat(&s, &fresh, NULL, NULL, &mismatch, &tmo);
    ASSERT_EQ_INT(fresh, 2);
    ASSERT_EQ_INT(mismatch, 1);
    ASSERT_EQ_INT(tmo, 1);
    svc_swap_done(&s);
    TEST_PASS();
}

typedef struct {
    svc_swap_t *s;
    int rc;
} drain_job_t;

static void *drain_worker(void *arg)
{
    drain_job_t *job = (drain_job_t *)arg;
    job->rc = svc_swap_begin(job->s);
    return NULL;
}

static void test_drain_waits_inflight(void)
{
    svc_swap_t s;
    uint32_t gen = 0;
    uint32_t late = 0;
    drain_job_t job;
    airy_thread_t th;

    TEST_BEGIN("drain_waits_inflight");
    ASSERT_EQ_INT(svc_swap_init(&s, 2000), 0);
    ASSERT_EQ_INT(svc_swap_enter(&s, &gen), 0);

    job.s = &s;
    job.rc = -1;
    ASSERT_EQ_INT(airy_thread_create(&th, drain_worker, &job), 0);
    airy_sleep_ms(60);

    /* 排空窗口内：新请求被拒，避免排空永不收敛 */
    ASSERT_EQ_INT(svc_swap_enter(&s, &late), AIRY_ERR_BUSY);

    ASSERT_EQ_INT(svc_swap_leave(&s, gen), 1);
    airy_thread_join(th, NULL);
    ASSERT_EQ_INT(job.rc, 0);

    /* 已排空：commit 推进世代并恢复接客 */
    ASSERT_EQ_INT(svc_swap_commit(&s), 0);
    ASSERT_EQ_INT(svc_swap_enter(&s, &late), 0);
    ASSERT_EQ_INT(late, 2);
    ASSERT_EQ_INT(svc_swap_leave(&s, late), 1);
    svc_swap_done(&s);
    TEST_PASS();
}

int main(void)
{
    printf("svc_swap unit tests\n");

    test_init_guards();
    test_enter_leave();
    test_busy_while_draining();
    test_abort_keeps_gen();
    test_commit_without_begin();
    test_drain_timeout_forced();
    test_drain_waits_inflight();

    printf("  %d/%d passed\n", g_tests_passed, g_tests_run);
    return g_tests_passed == g_tests_run ? 0 : 1;
}
