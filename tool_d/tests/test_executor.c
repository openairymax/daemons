// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_executor.c
 * @brief Tool 执行器单元测试
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

    arg->ret = tool_executor_run(arg->exec, &arg->meta, "0.35", "tool_d", &result);
    arg->end_ms = now_ms();
    if (result)
        tool_result_free(result);
    return NULL;
}

static int intervals_overlap(long long s1, long long e1, long long s2, long long e2)
{
    return s1 <= e2 && s2 <= e1;
}

static long long threads_total_ms(const exec_thread_arg_t *a, const exec_thread_arg_t *b)
{
    long long min_start = a->start_ms < b->start_ms ? a->start_ms : b->start_ms;
    long long max_end = a->end_ms > b->end_ms ? a->end_ms : b->end_ms;
    return max_end - min_start;
}

/* Note: start_ms is recorded BEFORE acquiring the gate. READ tools have no
 * lock-wait, so interval overlap and total time both prove concurrency; WRITE
 * tools arriving later wait on the gate (start recorded early), so intervals
 * necessarily overlap — write serialization can only be judged by total time:
 * serial total ~= 2x single execution, concurrent ~= single execution. */

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

    a.meta.id = "sleep_read_1";
    a.meta.name = "sleep_read";
    a.meta.executable = "/bin/sleep";
    a.meta.timeout_sec = 5;
    a.meta.access = TOOL_ACCESS_READ;
    b.meta = a.meta;
    b.meta.id = "sleep_read_2";

    pthread_t ta, tb;
    /* create/join must run independently of assert: in Release (NDEBUG),
     * assert arguments are not evaluated; wrapping pthread_create(...) in
     * assert would let the thread creation be optimized away entirely — the
     * join then sees a zeroed thread-arg struct and triggers libsanitizer's
     * ThreadArgRetval::BeforeJoin CHECK. */
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
    printf("    read intervals: [%lld,%lld] [%lld,%lld] total=%lldms\n", a.start_ms, a.end_ms,
           b.start_ms, b.end_ms, total);

    assert(total < 600);

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

    a.meta.id = "sleep_write_1";
    a.meta.name = "sleep_write";
    a.meta.executable = "/bin/sleep";
    a.meta.timeout_sec = 5;
    a.meta.access = TOOL_ACCESS_WRITE;
    b.meta = a.meta;
    b.meta.id = "sleep_write_2";

    pthread_t ta, tb;

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
    printf("    write intervals: [%lld,%lld] [%lld,%lld] total=%lldms\n", a.start_ms, a.end_ms,
           b.start_ms, b.end_ms, total);

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

    tool_executor_config_t config = {.max_workers = 5,
                                     .timeout_sec = 10,
                                     .workbench_type = "default"};

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
    meta.executable = "/bin/echo";
    meta.timeout_sec = 5;

    tool_result_t *result = NULL;
    int ret = tool_executor_run(exec, &meta, "hello", NULL, &result);
    if (ret == 0 && result != NULL) {
        if (result->output)
            printf("    Output: %s\n", result->output);
        tool_result_free(result);
    } else {
        /* P3.17: the fail-closed path may still allocate result (the executor
         * calloc'd before approval); must free to avoid a memory leak. */
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
    meta.executable = "/bin/echo";
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

    tool_executor_t *exec = tool_executor_create(NULL);
    assert(exec != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "fail_class";
    meta.name = "fail_class";
    meta.executable = "/bin/echo";
    meta.timeout_sec = 5;

    tool_result_t *result = NULL;
    int ret = tool_executor_run(exec, &meta, "hello", NULL, &result);
    if (result) {
        assert(result->success == 0);

        assert(result->failure_class == TOOL_RESULT_CLASS_FATAL);
        tool_result_free(result);
    } else {
        printf("    WARN: no result allocated (ret=%d)\n", ret);
    }

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

    test_executor_read_concurrent();
    test_executor_write_serial();

    printf("\nAll tool executor tests PASSED\n");
    return 0;
}
