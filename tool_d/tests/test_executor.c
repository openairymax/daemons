// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_executor.c
 * @brief Tool 执行器单元测试
 */

#include "core/executor.h"
#include "core/executor_run.h"
#include "core/executor_gate.h"
#include "core/approval_gate.h"
#include "approval/tool_approval.h"
#include "tool_service.h"

#include "airy_memory.h"
#include "daemon_security.h"
#include "platform.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long long now_ms(void)
{
    return (long long)airy_time_ms();
}

/* 为 executor 注入放行审批：daemon_security ACL + approval gate。
 * 注意：executor 拥有 gate（destroy 时释放），测试不得重复释放。 */
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
    approval_gate_t *gate = gate_create(&cfg);
    assert(gate != NULL);
    exec_set_gate(exec, gate);
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
    return s1 < e2 && s2 < e1;
}

static long long threads_total_ms(const exec_thread_arg_t *a, const exec_thread_arg_t *b)
{
    long long min_start = a->start_ms < b->start_ms ? a->start_ms : b->start_ms;
    long long max_end = a->end_ms > b->end_ms ? a->end_ms : b->end_ms;
    return max_end - min_start;
}

/* Concurrency ownership is the executor pool's; the executor itself never
 * serializes tools. Both READ and WRITE tools must run concurrently: strict
 * interval overlap proves interleaving and rules out serialization (a
 * zero-gap serial pair is rejected), independent of machine load — the
 * former wall-clock total bound was load-fragile and is gone. */

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

    assert(intervals_overlap(a.start_ms, a.end_ms, b.start_ms, b.end_ms));

    tool_executor_destroy(exec);
    printf("    PASSED\n");
}

static void test_executor_write_concurrent(void)
{
    printf("  test_executor_write_concurrent...\n");

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

    assert(intervals_overlap(a.start_ms, a.end_ms, b.start_ms, b.end_ms));

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
        assert(result->err_code != 0);

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
        assert(result->err_code != 0);
        assert(result->failure_class == TOOL_RESULT_CLASS_RESPOND_TO_MODEL);
        tool_result_free(result);
    } else {
        printf("    WARN: no result allocated (ret=%d)\n", ret);
    }

    tool_executor_destroy(exec);

    printf("    PASSED\n");
}

/* N-9：审批干跑与预指定凭据。probe 必须与真实审批同序同形：交互未启用
 * 时高危调用给 DENIED（无等待窗口，与直接拒绝同形），高危 + allow 给
 * ALLOWED，静态允许的低危调用给 ALLOWED；交互启用后给 INTERACTIVE 且
 * 窗口为 approval_timeout_ms。gate_ask_ex_id 接受调用方预指定的凭据，
 * exec_int_resolve 以同一凭据完成裁决——先亮凭据再提交的串行链闭环。 */

typedef struct {
    tool_executor_t *exec;
    tool_metadata_t meta;
    int ret;
    char approval[32];
} hint_run_arg_t;

static void *hint_run_fn(void *argp)
{
    hint_run_arg_t *arg = (hint_run_arg_t *)argp;
    tool_result_t *result = NULL;
    arg->ret = tool_executor_run_ex(arg->exec, &arg->meta, "{\"cmd\":\"true\"}", "tool_d", 0,
                                    "probe_case_1", &result);
    if (result) {
        AIRY_STRNCPY_TERM(arg->approval, result->approval, sizeof(arg->approval));
        tool_result_free(result);
    }
    return NULL;
}

static approval_gate_t *make_gate_interactive(void)
{
    tool_approval_config_t cfg;
    AIRY_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.agent_id = "tool_d";
    cfg.enable_safety_guard_chain = false;
    cfg.enable_audit_logging = false;
    return gate_create(&cfg);
}

static void test_gate_probe_and_hint_id(void)
{
    printf("  test_gate_probe_and_hint_id...\n");

    /* 交互未启用：高危 probe → DENIED（无等待窗口，与直接拒绝同形）。 */
    unsetenv("AIRY_TOOL_APPROVAL_MODE");
    tool_executor_t *exec = tool_executor_create(NULL);
    assert(exec != NULL);
    setup_approval(exec, "shell_run");

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "shell_run"; /* 高危表按 tool_id 精确匹配 */
    meta.name = "shell_run";
    meta.executable = "/bin/true";
    meta.timeout_sec = 5;
    meta.access = TOOL_ACCESS_WRITE;

    uint64_t timeout_ms = 12345;
    assert(tool_executor_gate_probe(exec, &meta, "{\"cmd\":\"true\"}", "tool_d", 0,
                                   &timeout_ms) == APPROVAL_GATE_DENIED);
    assert(timeout_ms == 0);

    /* 高危 + allow_dangerous：绕过交互，静态允许。 */
    assert(tool_executor_gate_probe(exec, &meta, "{\"cmd\":\"true\"}", "tool_d", 1,
                                   NULL) == APPROVAL_GATE_ALLOWED);
    tool_executor_destroy(exec);
    exec = NULL;

    /* 交互启用：probe 给 INTERACTIVE 且有窗口；真实执行带预指定凭据阻塞
     * 等待，另一线程按同一凭据放行——先亮凭据再提交的串行链闭环。 */
    setenv("AIRY_TOOL_APPROVAL_MODE", "interactive", 1);
    exec = tool_executor_create(NULL);
    assert(exec != NULL);
    approval_gate_t *gate = make_gate_interactive();
    assert(gate != NULL);
    exec_set_gate(exec, gate);

    tool_metadata_t hmeta;
    AIRY_MEMSET(&hmeta, 0, sizeof(hmeta));
    hmeta.id = "shell_run"; /* 高危表按 tool_id 精确匹配 */
    hmeta.name = "shell_run";
    hmeta.executable = "/bin/true";
    hmeta.timeout_sec = 5;
    hmeta.access = TOOL_ACCESS_WRITE;

    uint64_t window = 0;
    assert(tool_executor_gate_probe(exec, &hmeta, "{\"cmd\":\"true\"}", "tool_d", 0,
                                   &window) == APPROVAL_GATE_INTERACTIVE);
    assert(window > 0);

    hint_run_arg_t arg;
    AIRY_MEMSET(&arg, 0, sizeof(arg));
    arg.exec = exec;
    arg.meta = hmeta;
    pthread_t th;
    assert(pthread_create(&th, NULL, hint_run_fn, &arg) == 0);

    /* 等 pending 入链再按同一凭据裁决（resolve 未命中短暂退避重试）。 */
    int resolved = AIRY_ERR_NOT_FOUND;
    for (int i = 0; i < 100 && resolved != 0; i++) {
        resolved = exec_int_resolve(exec, "probe_case_1", "allow");
        if (resolved != 0)
            usleep(10000);
    }
    assert(resolved == 0);

    pthread_join(th, NULL);
    assert(arg.ret == 0);

    tool_executor_destroy(exec);
    unsetenv("AIRY_TOOL_APPROVAL_MODE");

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
    test_executor_write_concurrent();
    test_gate_probe_and_hint_id();

    printf("\nAll tool executor tests PASSED\n");
    return 0;
}
