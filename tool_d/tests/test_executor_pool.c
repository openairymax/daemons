// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_executor_pool.c
 * @brief R1-a (0.1.17): 执行面隔离池验收测试。
 *
 * 覆盖三类验收（0.1.17 §3.1）：
 *   1. 并发两会话互不影响：一个会话注入超预算工具（CANCELED 兜底），
 *      另一会话的快速工具正常完成且先于超时返回；
 *   2. 取消返回明确错误码 AIRY_ERR_CANCELED + 合成 NORMAL_FAIL 结果；
 *   3. 队列满背压快失败 AIRY_ERR_BUSY；destroy 对 detach 在途 job 的
 *      drain 语义（不悬挂、不 UAF）。
 */

#include "airy_memory.h"
#include "daemon_security.h"
#include "error.h"
#include "executor.h"
#include "executor_pool.h"

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

/* 放行审批（同 test_executor.c 的 fail-closed 语义：ACL + approval_ctx） */
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
    executor_pool_t *pool;
    tool_metadata_t meta;
    const char *params;
    tool_result_t *result;
    int ret;
    long long start_ms;
    long long end_ms;
} pool_thread_arg_t;

static void *pool_thread_fn(void *argp)
{
    pool_thread_arg_t *arg = (pool_thread_arg_t *)argp;
    arg->start_ms = now_ms();
    arg->result = NULL;
    arg->ret = executor_pool_run(arg->pool, &arg->meta, arg->params, "tool_d", &arg->result);
    arg->end_ms = now_ms();
    return NULL;
}

static void meta_fill(tool_metadata_t *meta, const char *id, const char *name,
                      const char *executable, int timeout_sec)
{
    AIRY_MEMSET(meta, 0, sizeof(*meta));
    meta->id = (char *)id;
    meta->name = (char *)name;
    meta->executable = (char *)executable;
    meta->timeout_sec = timeout_sec;
    meta->access = TOOL_ACCESS_READ;
}

/* 用例 1+2：并发两会话互不影响 + CANCELED 明确错误码 */
static void test_pool_concurrent_isolation(void)
{
    printf("  test_pool_concurrent_isolation...\n");

    unsetenv("AIRY_TOOL_EXEC_WORKERS");
    setenv("AIRY_TOOL_WAIT_BUDGET_MS", "1500", 1);

    tool_executor_config_t cfg;
    AIRY_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.timeout_sec = 30;
    tool_executor_t *exec = tool_executor_create_ex(&cfg);
    assert(exec != NULL);
    setup_approval(exec, "pool_sleep");
    daemon_security_add_acl_rule("tool_d", "pool_echo", true);

    executor_pool_t *pool = executor_pool_new(exec);
    assert(pool != NULL);

    pool_thread_arg_t slow;
    AIRY_MEMSET(&slow, 0, sizeof(slow));
    slow.pool = pool;
    slow.params = "3";
    meta_fill(&slow.meta, "pool_slow_session", "pool_sleep", "/bin/sleep", 30);

    pthread_t ta;
    int rc = pthread_create(&ta, NULL, pool_thread_fn, &slow);
    assert(rc == 0);

    /* 确保慢会话已入队并开跑，再注入快速会话 */
    struct timespec req = {0, 200 * 1000 * 1000};
    nanosleep(&req, NULL);

    pool_thread_arg_t fast;
    AIRY_MEMSET(&fast, 0, sizeof(fast));
    fast.pool = pool;
    fast.params = "pool_ok";
    meta_fill(&fast.meta, "pool_fast_session", "pool_echo", "/bin/echo", 30);

    long long fast_end_before_slow = 0;
    fast.start_ms = now_ms();
    fast.ret = executor_pool_run(pool, &fast.meta, fast.params, "tool_d", &fast.result);
    fast.end_ms = now_ms();
    fast_end_before_slow = fast.end_ms;

    assert(fast.ret == AIRY_OK);
    assert(fast.result != NULL);
    assert(fast.result->success == 1);
    assert(fast.result->output != NULL && strstr(fast.result->output, "pool_ok") != NULL);
    printf("    fast session ok in %lldms\n", fast.end_ms - fast.start_ms);

    rc = pthread_join(ta, NULL);
    assert(rc == 0);
    tool_result_free(fast.result);
    fast.result = NULL;

    /* 慢会话：预算（1500ms）耗尽 → 明确取消错误码 + 合成 NORMAL_FAIL 结果 */
    assert(slow.ret == AIRY_ERR_CANCELED);
    assert(slow.result != NULL);
    assert(slow.result->success == 0);
    assert(slow.result->error != NULL &&
           strstr(slow.result->error, "canceled") != NULL);
    assert(slow.result->failure_class == TOOL_RESULT_CLASS_NORMAL_FAIL);
    assert(slow.result->exit_code == -1);
    printf("    slow session canceled after %lldms (budget 1500ms)\n",
           slow.end_ms - slow.start_ms);
    tool_result_free(slow.result);
    slow.result = NULL;

    /* 互不影响判据：快速会话先于慢会话取消点完成 */
    assert(fast_end_before_slow < slow.end_ms);

    /* destroy drain：慢 job（sleep 3）仍在途，free 必须等待其落定 */
    long long t0 = now_ms();
    executor_pool_free(pool);
    printf("    destroy drained detached job in %lldms\n", now_ms() - t0);

    tool_executor_destroy(exec);
    unsetenv("AIRY_TOOL_WAIT_BUDGET_MS");
    printf("    PASSED\n");
}

#define BUSY_SUBMITTERS 65

typedef struct {
    executor_pool_t *pool;
    tool_metadata_t meta;
    const char *params;
    int ret;
} busy_arg_t;

static void *busy_thread_fn(void *argp)
{
    busy_arg_t *arg = (busy_arg_t *)argp;
    tool_result_t *res = NULL;
    arg->ret = executor_pool_run(arg->pool, &arg->meta, arg->params, "tool_d", &res);
    if (res)
        tool_result_free(res);
    return NULL;
}

/* 用例 3：队列满背压快失败 BUSY */
static void test_pool_busy_backpressure(void)
{
    printf("  test_pool_busy_backpressure...\n");

    setenv("AIRY_TOOL_EXEC_WORKERS", "1", 1);
    unsetenv("AIRY_TOOL_WAIT_BUDGET_MS");

    tool_executor_config_t cfg;
    AIRY_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.timeout_sec = 30;
    tool_executor_t *exec = tool_executor_create_ex(&cfg);
    assert(exec != NULL);
    setup_approval(exec, "pool_busy");

    executor_pool_t *pool = executor_pool_new(exec);
    assert(pool != NULL);

    /* 先占住唯一 worker（sleep 2 真实占道），再并发灌满 64 深队列，第 65 个 BUSY */
    tool_metadata_t slow_meta;
    meta_fill(&slow_meta, "pool_busy_slow", "pool_busy", "/bin/sleep", 30);
    busy_arg_t holder = {.pool = pool, .meta = slow_meta, .params = "2", .ret = -1000};
    pthread_t th;
    int rc = pthread_create(&th, NULL, busy_thread_fn, &holder);
    assert(rc == 0);

    struct timespec req = {0, 200 * 1000 * 1000};
    nanosleep(&req, NULL);

    tool_metadata_t fast_meta;
    meta_fill(&fast_meta, "pool_busy_fast", "pool_busy", "/bin/echo", 30);

    busy_arg_t args[BUSY_SUBMITTERS];
    pthread_t tids[BUSY_SUBMITTERS];
    for (int i = 0; i < BUSY_SUBMITTERS; ++i) {
        args[i].pool = pool;
        args[i].meta = fast_meta;
        args[i].params = "ok";
        args[i].ret = -1000;
        int c = pthread_create(&tids[i], NULL, busy_thread_fn, &args[i]);
        assert(c == 0);
    }
    for (int i = 0; i < BUSY_SUBMITTERS; ++i)
        pthread_join(tids[i], NULL);

    int busy_cnt = 0;
    int ok_cnt = 0;
    for (int i = 0; i < BUSY_SUBMITTERS; ++i) {
        if (args[i].ret == AIRY_ERR_BUSY)
            busy_cnt++;
        else if (args[i].ret == AIRY_OK)
            ok_cnt++;
    }
    printf("    submitted=%d ok=%d busy=%d (queue cap 64)\n", BUSY_SUBMITTERS, ok_cnt, busy_cnt);
    assert(busy_cnt >= 1);
    assert(ok_cnt + busy_cnt == BUSY_SUBMITTERS);

    pthread_join(th, NULL);
    assert(holder.ret == AIRY_OK);

    executor_pool_free(pool);
    tool_executor_destroy(exec);
    unsetenv("AIRY_TOOL_EXEC_WORKERS");
    printf("    PASSED\n");
}

/* 用例 4：destroy 对 detach 在途 job 的 drain（不悬挂、不 UAF） */
static void test_pool_destroy_drain(void)
{
    printf("  test_pool_destroy_drain...\n");

    unsetenv("AIRY_TOOL_EXEC_WORKERS");
    setenv("AIRY_TOOL_WAIT_BUDGET_MS", "500", 1);

    tool_executor_config_t cfg;
    AIRY_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.timeout_sec = 30;
    tool_executor_t *exec = tool_executor_create_ex(&cfg);
    assert(exec != NULL);
    setup_approval(exec, "pool_drain");

    executor_pool_t *pool = executor_pool_new(exec);
    assert(pool != NULL);

    tool_metadata_t meta;
    meta_fill(&meta, "pool_drain_tool", "pool_drain", "/bin/sleep", 30);
    tool_result_t *res = NULL;
    long long t0 = now_ms();
    int ret = executor_pool_run(pool, &meta, "2", "tool_d", &res);
    long long waited = now_ms() - t0;

    assert(ret == AIRY_ERR_CANCELED);
    assert(waited >= 400 && waited < 2000);
    tool_result_free(res);
    res = NULL;

    /* job 已 detach 且仍在跑（sleep 2）：free 必须等它落定后返回 */
    t0 = now_ms();
    executor_pool_free(pool);
    long long drained = now_ms() - t0;
    printf("    canceled after %lldms, destroy drained in %lldms\n", waited, drained);
    assert(drained >= 1000);

    tool_executor_destroy(exec);
    unsetenv("AIRY_TOOL_WAIT_BUDGET_MS");
    printf("    PASSED\n");
}

int main(void)
{
    printf("test_executor_pool:\n");
    test_pool_concurrent_isolation();
    test_pool_busy_backpressure();
    test_pool_destroy_drain();
    printf("ALL PASSED\n");
    return 0;
}
