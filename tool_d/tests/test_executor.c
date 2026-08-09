// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file test_executor.c
 * @brief Tool 执行器单元测试
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "executor.h"
#include "tool_service.h"

#include "airy_memory.h"
#include "daemon_security.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========== 改进1（P1d）：并行工具并发门控测试辅助 ========== */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 为 executor 注入放行审批：daemon_security ACL + approval_ctx。
 * 注意：executor 拥有 approval_ctx（destroy 时释放），测试不得重复释放。 */
static void setup_approval(tool_executor_t *exec, const char *tool_name)
{
    daemon_security_init(NULL, NULL);
    int ar = daemon_security_add_acl_rule("tool_d", tool_name, true);
    assert(ar == 0);

    tool_approval_config_t cfg;
    AIRY_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.agent_id = "tool_d";
    cfg.enable_safety_guard_chain = false;
    cfg.enable_audit_logging = false;
    tool_approval_ctx_t *ctx = tool_approval_create(&cfg);
    assert(ctx != NULL);
    tool_executor_set_approval_ctx(exec, ctx);
}

typedef struct {
    tool_executor_t *exec;
    tool_metadata_t meta;
    long long start_ms;
    long long end_ms;
    int ret;
} exec_thread_arg_t;

static void *exec_thread_fn(void *argp)
{
    exec_thread_arg_t *arg = (exec_thread_arg_t *)argp;
    arg->start_ms = now_ms();
    tool_result_t *result = NULL;
    /* agent_id 显式传 "tool_d"（与 ACL 规则一致） */
    arg->ret = tool_executor_run(arg->exec, &arg->meta, "0.35", "tool_d", &result);
    arg->end_ms = now_ms();
    if (result)
        tool_result_free(result);
    return NULL;
}

/* 两个执行区间是否重叠（含边界） */
static int intervals_overlap(long long s1, long long e1, long long s2, long long e2)
{
    return s1 <= e2 && s2 <= e1;
}

/* 两个线程的总耗时（从最早 start 到最晚 end） */
static long long threads_total_ms(const exec_thread_arg_t *a, const exec_thread_arg_t *b)
{
    long long min_start = a->start_ms < b->start_ms ? a->start_ms : b->start_ms;
    long long max_end = a->end_ms > b->end_ms ? a->end_ms : b->end_ms;
    return max_end - min_start;
}

/* 说明：start_ms 记录于门控获取之前。READ 工具无等锁等待，区间重叠与总耗时
 * 均能证明并发；WRITE 工具后到者等待门控（start 提前记录），区间必然重叠，
 * 因此写串行只能用总耗时判定：串行总耗时 ≈ 2×单次执行，并发 ≈ 单次执行。 */

static void test_executor_read_concurrent(void)
{
    printf("  test_executor_read_concurrent...\n");

    tool_executor_t *exec = tool_executor_create(NULL);
    assert(exec != NULL);
    setup_approval(exec, "sleep_read");

    exec_thread_arg_t a, b;
    AIRY_MEMSET(&a, 0, sizeof(a));
    AIRY_MEMSET(&b, 0, sizeof(b));
    a.exec = exec;
    b.exec = exec;
    /* 两个只读工具（TOOL_ACCESS_READ）：应并发执行（读门并行） */
    a.meta.id = "sleep_read_1";
    a.meta.name = "sleep_read";
    a.meta.executable = "/usr/bin/sleep";
    a.meta.timeout_sec = 5;
    a.meta.access = TOOL_ACCESS_READ;
    b.meta = a.meta;
    b.meta.id = "sleep_read_2";

    pthread_t ta, tb;
    /* create/join 必须独立于 assert 执行：Release（NDEBUG）下 assert 参数不求值，
     * 若以 assert(pthread_create(...)) 包裹，线程创建会被整体优化掉——join 时
     * 线程参数结构为 0，触发 libsanitizer ThreadArgRetval::BeforeJoin CHECK。 */
    int rc_ta = pthread_create(&ta, NULL, exec_thread_fn, &a);
    assert(rc_ta == 0);
    int rc_tb = pthread_create(&tb, NULL, exec_thread_fn, &b);
    assert(rc_tb == 0);
    int rc_ja = pthread_join(ta, NULL);
    assert(rc_ja == 0);
    int rc_jb = pthread_join(tb, NULL);
    assert(rc_jb == 0);

    assert(a.ret == AIRY_OK && b.ret == AIRY_OK);
    long long total = threads_total_ms(&a, &b);
    printf("    read intervals: [%lld,%lld] [%lld,%lld] total=%lldms\n",
           a.start_ms, a.end_ms, b.start_ms, b.end_ms, total);
    /* 并发：总耗时显著小于两次 sleep（0.35s×2=700ms，阈值 600ms） */
    assert(total < 600);
    /* 两执行区间应真实重叠（无门控等待） */
    assert(intervals_overlap(a.start_ms, a.end_ms, b.start_ms, b.end_ms));

    tool_executor_destroy(exec);
    printf("    PASSED\n");
}

static void test_executor_write_serial(void)
{
    printf("  test_executor_write_serial...\n");

    tool_executor_t *exec = tool_executor_create(NULL);
    assert(exec != NULL);
    setup_approval(exec, "sleep_write");

    exec_thread_arg_t a, b;
    AIRY_MEMSET(&a, 0, sizeof(a));
    AIRY_MEMSET(&b, 0, sizeof(b));
    a.exec = exec;
    b.exec = exec;
    /* 两个写工具（TOOL_ACCESS_WRITE）：必须互斥串行（写门互斥） */
    a.meta.id = "sleep_write_1";
    a.meta.name = "sleep_write";
    a.meta.executable = "/usr/bin/sleep";
    a.meta.timeout_sec = 5;
    a.meta.access = TOOL_ACCESS_WRITE;
    b.meta = a.meta;
    b.meta.id = "sleep_write_2";

    pthread_t ta, tb;
    /* 同 read 并发：create/join 独立于 assert（NDEBUG 下 assert 参数不求值） */
    int rc_ta = pthread_create(&ta, NULL, exec_thread_fn, &a);
    assert(rc_ta == 0);
    int rc_tb = pthread_create(&tb, NULL, exec_thread_fn, &b);
    assert(rc_tb == 0);
    int rc_ja = pthread_join(ta, NULL);
    assert(rc_ja == 0);
    int rc_jb = pthread_join(tb, NULL);
    assert(rc_jb == 0);

    assert(a.ret == AIRY_OK && b.ret == AIRY_OK);
    long long total = threads_total_ms(&a, &b);
    printf("    write intervals: [%lld,%lld] [%lld,%lld] total=%lldms\n",
           a.start_ms, a.end_ms, b.start_ms, b.end_ms, total);
    /* 串行：总耗时 ≥ 两次 sleep（0.35s×2=700ms，容差 150ms） */
    assert(total >= 700 - 150);

    tool_executor_destroy(exec);
    printf("    PASSED\n");
}

static void test_executor_create_destroy(void)
{
    printf("  test_executor_create_destroy...\n");

    tool_executor_t *exec = tool_executor_create(NULL);
    assert(exec != NULL);

    tool_executor_destroy(exec);

    printf("    PASSED\n");
}

static void test_executor_config(void)
{
    printf("  test_executor_config...\n");

    tool_executor_config_t config = {
        .max_workers = 5, .timeout_sec = 10, .workbench_type = "default"};

    tool_executor_t *exec = tool_executor_create(&config);
    assert(exec != NULL);

    tool_executor_destroy(exec);

    printf("    PASSED\n");
}

static void test_executor_run(void)
{
    printf("  test_executor_run...\n");

    tool_executor_t *exec = tool_executor_create(NULL);
    assert(exec != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "test_echo";
    meta.name = "echo_test";
    meta.executable = "/usr/bin/echo";
    meta.timeout_sec = 5;

    tool_result_t *result = NULL;
    int ret = tool_executor_run(exec, &meta, "hello", NULL, &result);
    if (ret == 0 && result != NULL) {
        if (result->output)
            printf("    Output: %s\n", result->output);
        tool_result_free(result);
    } else {
        /* P3.17: fail-closed 路径仍可能分配 result（executor 在审批前已 calloc），
         * 必须释放避免内存泄漏。*/
        if (result)
            tool_result_free(result);
        printf("    Execution skipped or failed (expected in test env, ret=%d)\n", ret);
    }

    tool_executor_destroy(exec);

    printf("    PASSED\n");
}

static void test_executor_run_async(void)
{
    printf("  test_executor_run_async...\n");

    tool_executor_t *exec = tool_executor_create(NULL);
    assert(exec != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "test_echo_async";
    meta.name = "echo_async";
    meta.executable = "/usr/bin/echo";
    meta.timeout_sec = 5;

    tool_result_t *result = NULL;
    int ret = tool_executor_run_async(exec, &meta, "hello", NULL, NULL, NULL, &result);
    if (ret == 0 && result != NULL) {
        tool_result_free(result);
    } else {
        if (result)
            tool_result_free(result);
        printf("    Async execution skipped or failed (expected in test env, ret=%d)\n", ret);
    }

    tool_executor_destroy(exec);

    printf("    PASSED\n");
}

static void test_executor_failure_class(void)
{
    printf("  test_executor_failure_class...\n");

    /* approval_ctx 未注入 → fail-closed：结果分级必须为 FATAL（改进3） */
    tool_executor_t *exec = tool_executor_create(NULL);
    assert(exec != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "fail_class";
    meta.name = "fail_class";
    meta.executable = "/usr/bin/echo";
    meta.timeout_sec = 5;

    tool_result_t *result = NULL;
    int ret = tool_executor_run(exec, &meta, "hello", NULL, &result);
    if (result) {
        assert(result->success == 0);
        /* 安全系统未配置 = 致命（fail-closed），非普通失败/回传继续 */
        assert(result->failure_class == TOOL_RESULT_CLASS_FATAL);
        tool_result_free(result);
    } else {
        printf("    WARN: no result allocated (ret=%d)\n", ret);
    }

    /* 缺 executable → 回传继续（RESPOND_TO_MODEL），任务不终止 */
    tool_metadata_t bad_meta;
    AIRY_MEMSET(&bad_meta, 0, sizeof(bad_meta));
    bad_meta.id = "no_exec";
    bad_meta.name = "no_exec";
    bad_meta.executable = "";
    result = NULL;
    ret = tool_executor_run(exec, &bad_meta, "{}", NULL, &result);
    if (result) {
        assert(result->success == 0);
        assert(result->failure_class == TOOL_RESULT_CLASS_RESPOND_TO_MODEL);
        tool_result_free(result);
    } else {
        printf("    WARN: no result allocated (ret=%d)\n", ret);
    }

    tool_executor_destroy(exec);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Tool Executor Unit Tests\n");
    printf("=========================================\n");

    test_executor_create_destroy();
    test_executor_config();
    test_executor_run();
    test_executor_run_async();
    test_executor_failure_class();
    /* P1d：并行工具并发门控（READ 并发 / WRITE 串行） */
    test_executor_read_concurrent();
    test_executor_write_serial();

    printf("\nAll tool executor tests PASSED\n");
    return 0;
}
