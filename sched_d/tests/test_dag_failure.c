// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_dag_failure.c
 * @brief DAG 失败语义测试：失败级联 / 取消 / 分级失败 / 瞬时重试
 */

#include "test_dag_internal.h"

int test_dag_failure_cascade(void)
{
    printf("=== test_dag_failure_cascade ===\n");

    sched_service_t *svc = make_service();
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }
    g_exec_count = 0;
    g_fail_goal = NULL;
    g_block = 0;
    g_fail_goal = "boom-B";

    const char *dag_json = "{\"name\":\"fail\",\"nodes\":["
                           "{\"id\":\"A\",\"goal\":\"ok-A\",\"depends\":[]},"
                           "{\"id\":\"B\",\"goal\":\"boom-B\",\"depends\":[\"A\"]},"
                           "{\"id\":\"C\",\"goal\":\"never-C\",\"depends\":[\"B\"]}"
                           "]}";
    char *dag_id = NULL;
    int ret = sched_service_submit_dag(svc, dag_json, &dag_id);
    if (ret != AIRY_SUCCESS || !dag_id) {
        printf("  FAILED: submit rc=%d\n", ret);
        sched_service_destroy(svc);
        return 1;
    }

    if (wait_dag_terminal(svc, dag_id, 5000) != 0) {
        printf("  FAILED: dag timeout\n");
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    char st[32];
    get_dag_status_str(svc, dag_id, st, sizeof(st));
    if (strcmp(st, "failed") != 0) {
        printf("  FAILED: dag status=%s (expect failed)\n", st);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    for (size_t i = 0; i < g_exec_count; i++) {
        if (strstr(g_exec_log[i], "never-C")) {
            printf("  FAILED: node C executed after B failed\n");
            AIRY_FREE(dag_id);
            sched_service_destroy(svc);
            return 1;
        }
    }

    printf("  PASSED (fail-fast: B failed, C canceled; exec=%zu)\n\n", g_exec_count);
    AIRY_FREE(dag_id);
    sched_service_destroy(svc);
    return 0;
}

int test_dag_cancel(void)
{
    printf("=== test_dag_cancel ===\n");

    sched_service_t *svc = make_service();
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }
    g_exec_count = 0;
    g_fail_goal = NULL;
    g_block = 1;

    const char *dag_json = "{\"name\":\"cancel\",\"nodes\":["
                           "{\"id\":\"A\",\"goal\":\"block-A\",\"depends\":[]},"
                           "{\"id\":\"B\",\"goal\":\"never-B\",\"depends\":[\"A\"]}"
                           "]}";
    char *dag_id = NULL;
    int ret = sched_service_submit_dag(svc, dag_json, &dag_id);
    if (ret != AIRY_SUCCESS || !dag_id) {
        printf("  FAILED: submit rc=%d\n", ret);
        sched_service_destroy(svc);
        return 1;
    }

    TEST_SLEEP_MS(200);
    ret = sched_service_cancel_dag(svc, dag_id);
    if (ret != AIRY_SUCCESS) {
        printf("  FAILED: cancel_dag rc=%d\n", ret);
        g_block = 0;
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    g_block = 0;

    char st[32];
    get_dag_status_str(svc, dag_id, st, sizeof(st));
    if (strcmp(st, "canceled") != 0) {
        printf("  FAILED: dag status=%s (expect canceled)\n", st);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    /* P0-1 verification: when canceling, a RUNNING node (A) must normalize to
     * canceled after completion, and its output must be discarded (the header
     * promises "no more on-screen output"). */
    char *json = NULL;
    if (sched_service_get_dag(svc, dag_id, &json) != AIRY_SUCCESS || !json) {
        printf("  FAILED: get_dag after cancel\n");
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    const char *a_node = strstr(json, "\"goal\":\"block-A\"");
    if (a_node == NULL || strstr(a_node, "\"status\":\"canceled\"") == NULL) {
        printf("  FAILED: running node A not normalized to canceled\n");
        AIRY_FREE(json);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    if (strstr(json, "done[block-A]") != NULL) {
        printf("  FAILED: canceled node A output not discarded\n");
        AIRY_FREE(json);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    AIRY_FREE(json);

    printf("  PASSED (dag canceled while node running; node A normalized to canceled)\n\n");
    AIRY_FREE(dag_id);
    sched_service_destroy(svc);
    return 0;
}

/* ---- Case 9: ordinary failure does not cascade the whole graph
 * (improvement 3, dag_fatal_cascade=true production default) ----
 * B fails ordinarily -> only its unreachable downstream C is canceled; the
 * independent branch A completes normally; the graph finally converges to
 * FAILED truthfully (partial failure). */
int test_dag_normal_failure_no_cascade(void)
{
    printf("=== test_dag_normal_failure_no_cascade ===\n");

    sched_service_t *svc = make_service_graded(true, 0);
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }
    g_exec_count = 0;
    g_fail_goal = "boom-B";
    g_fatal_goal = NULL;
    g_flaky_goal = NULL;
    g_flaky_left = 0;
    g_block = 0;

    const char *dag_json = "{\"name\":\"graded\",\"nodes\":["
                           "{\"id\":\"A\",\"goal\":\"ok-A\",\"depends\":[]},"
                           "{\"id\":\"B\",\"goal\":\"boom-B\",\"depends\":[\"A\"]},"
                           "{\"id\":\"C\",\"goal\":\"never-C\",\"depends\":[\"B\"]}"
                           "]}";
    char *dag_id = NULL;
    int ret = sched_service_submit_dag(svc, dag_json, &dag_id);
    if (ret != AIRY_SUCCESS || !dag_id) {
        printf("  FAILED: submit rc=%d\n", ret);
        sched_service_destroy(svc);
        return 1;
    }
    if (wait_dag_terminal(svc, dag_id, 5000) != 0) {
        printf("  FAILED: dag timeout\n");
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    char st[32];
    get_dag_status_str(svc, dag_id, st, sizeof(st));
    if (strcmp(st, "failed") != 0) {
        printf("  FAILED: dag status=%s (expect failed)\n", st);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    if (g_exec_count != 2) {
        printf("  FAILED: executor called %zu times (expect 2: A+B)\n", g_exec_count);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    int a_ran = 0, c_ran = 0;
    for (size_t i = 0; i < g_exec_count; i++) {
        if (strstr(g_exec_log[i], "ok-A"))
            a_ran = 1;
        if (strstr(g_exec_log[i], "never-C"))
            c_ran = 1;
    }
    if (!a_ran) {
        printf("  FAILED: independent branch A did not run after B failure\n");
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    if (c_ran) {
        printf("  FAILED: node C (dep of failed B) executed\n");
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    printf("  PASSED (B failed, A continued, C unreachable canceled; exec=%zu)\n\n", g_exec_count);
    AIRY_FREE(dag_id);
    sched_service_destroy(svc);
    return 0;
}

/* ---- Case 10: FATAL failure cascades whole-graph cancellation
 * (improvement 3, fail-closed) ----
 * In the parallel batch, B triggers FATAL (OOM) -> the whole graph FAILED,
 * unfinished nodes all canceled, and the independent branch A's result is
 * also discarded (the graph has converged). */
int test_dag_fatal_cascade_whole(void)
{
    printf("=== test_dag_fatal_cascade_whole ===\n");

    sched_service_t *svc = make_service_graded(true, 2);
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }
    g_exec_count = 0;
    g_fail_goal = NULL;
    g_fatal_goal = "fatal-B";
    g_flaky_goal = NULL;
    g_flaky_left = 0;
    g_block = 0;

    const char *dag_json = "{\"name\":\"fatal\",\"nodes\":["
                           "{\"id\":\"A\",\"goal\":\"ok-A\",\"depends\":[]},"
                           "{\"id\":\"B\",\"goal\":\"fatal-B\",\"depends\":[]},"
                           "{\"id\":\"C\",\"goal\":\"never-C\",\"depends\":[\"A\",\"B\"]}"
                           "]}";
    char *dag_id = NULL;
    int ret = sched_service_submit_dag(svc, dag_json, &dag_id);
    if (ret != AIRY_SUCCESS || !dag_id) {
        printf("  FAILED: submit rc=%d\n", ret);
        sched_service_destroy(svc);
        return 1;
    }
    if (wait_dag_terminal(svc, dag_id, 5000) != 0) {
        printf("  FAILED: dag timeout\n");
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    char st[32];
    get_dag_status_str(svc, dag_id, st, sizeof(st));
    if (strcmp(st, "failed") != 0) {
        printf("  FAILED: dag status=%s (expect failed)\n", st);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    /* A 与 B 为两个无依赖的并行节点：B 触发 FATAL 时 A 可能在运行中被
     * 级联取消（exec=1），也可能已执行完（exec=2）——两种都是正确行为
     * （pending/未决节点在 FATAL 后统一 cancel）。只断言 C 永不执行、
     * 图终态为 failed，不把 A 的执行次序当硬性约定（#46 Linux/macOS
     * 时序竞态实证：B 先于 A 派发时 exec 恰为 1）。 */
    if (g_exec_count != 1 && g_exec_count != 2) {
        printf("  FAILED: executor called %zu times (expect 1 or 2: A±B ran, C must be canceled)\n",
               g_exec_count);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    for (size_t i = 0; i < g_exec_count; i++) {
        if (strstr(g_exec_log[i], "never-C")) {
            printf("  FAILED: node C executed after FATAL\n");
            AIRY_FREE(dag_id);
            sched_service_destroy(svc);
            return 1;
        }
    }

    printf("  PASSED (FATAL aborted whole graph, C canceled; exec=%zu)\n\n", g_exec_count);
    AIRY_FREE(dag_id);
    sched_service_destroy(svc);
    return 0;
}

/* ---- Case 11: transient failure graded retry succeeds (improvement 4) ----
 * A fails transiently (timeout) the first 2 times -> exponential-backoff
 * retry -> succeeds on the 3rd; the graph is COMPLETED, node
 * retry_count==2. */
int test_dag_transient_retry(void)
{
    printf("=== test_dag_transient_retry ===\n");

    sched_service_t *svc = make_service_graded(true, 0);
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }
    g_exec_count = 0;
    g_fail_goal = NULL;
    g_fatal_goal = NULL;
    g_flaky_goal = "flaky-A";
    g_flaky_left = 2;
    g_block = 0;

    const char *dag_json = "{\"name\":\"retry\",\"nodes\":["
                           "{\"id\":\"A\",\"goal\":\"flaky-A\",\"depends\":[],"
                           "\"max_retries\":2,\"retry_delay_ms\":30}"
                           "]}";
    char *dag_id = NULL;
    int ret = sched_service_submit_dag(svc, dag_json, &dag_id);
    if (ret != AIRY_SUCCESS || !dag_id) {
        printf("  FAILED: submit rc=%d\n", ret);
        sched_service_destroy(svc);
        return 1;
    }
    if (wait_dag_terminal(svc, dag_id, 5000) != 0) {
        printf("  FAILED: dag timeout\n");
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    char st[32];
    get_dag_status_str(svc, dag_id, st, sizeof(st));
    if (strcmp(st, "completed") != 0) {
        printf("  FAILED: dag status=%s (expect completed)\n", st);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    if (g_exec_count != 3) {
        printf("  FAILED: executor called %zu times (expect 3: 2 retries + success)\n",
               g_exec_count);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    char *json = NULL;
    if (sched_service_get_dag(svc, dag_id, &json) == AIRY_SUCCESS && json) {
        if (strstr(json, "\"retry_count\":2") == NULL ||
            strstr(json, "\"max_retries\":2") == NULL) {
            printf("  FAILED: retry metadata missing in dag status: %s\n", json);
            AIRY_FREE(json);
            AIRY_FREE(dag_id);
            sched_service_destroy(svc);
            return 1;
        }
        AIRY_FREE(json);
    }

    printf("  PASSED (transient retried 2x, then succeeded; exec=%zu)\n\n", g_exec_count);
    AIRY_FREE(dag_id);
    sched_service_destroy(svc);
    return 0;
}

/* ---- Case 12: transient retries exhausted -> ordinary failure
 * (improvement 4) ----
 * A keeps failing transiently, max_retries=1 -> after 1 retry it FAILED and
 * the graph FAILED. */
int test_dag_transient_retry_exhausted(void)
{
    printf("=== test_dag_transient_retry_exhausted ===\n");

    sched_service_t *svc = make_service_graded(true, 0);
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }
    g_exec_count = 0;
    g_fail_goal = NULL;
    g_fatal_goal = NULL;
    g_flaky_goal = "flaky-A";
    g_flaky_left = 100;
    g_block = 0;

    const char *dag_json = "{\"name\":\"retryx\",\"nodes\":["
                           "{\"id\":\"A\",\"goal\":\"flaky-A\",\"depends\":[],"
                           "\"max_retries\":1,\"retry_delay_ms\":30}"
                           "]}";
    char *dag_id = NULL;
    int ret = sched_service_submit_dag(svc, dag_json, &dag_id);
    if (ret != AIRY_SUCCESS || !dag_id) {
        printf("  FAILED: submit rc=%d\n", ret);
        sched_service_destroy(svc);
        return 1;
    }
    if (wait_dag_terminal(svc, dag_id, 5000) != 0) {
        printf("  FAILED: dag timeout\n");
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    char st[32];
    get_dag_status_str(svc, dag_id, st, sizeof(st));
    if (strcmp(st, "failed") != 0) {
        printf("  FAILED: dag status=%s (expect failed)\n", st);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    if (g_exec_count != 2) {
        printf("  FAILED: executor called %zu times (expect 2: 1 attempt + 1 retry)\n",
               g_exec_count);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    char *json = NULL;
    if (sched_service_get_dag(svc, dag_id, &json) == AIRY_SUCCESS && json) {
        if (strstr(json, "\"retry_count\":1") == NULL) {
            printf("  FAILED: retry_count not 1 in dag status: %s\n", json);
            AIRY_FREE(json);
            AIRY_FREE(dag_id);
            sched_service_destroy(svc);
            return 1;
        }
        AIRY_FREE(json);
    }

    printf("  PASSED (retries exhausted after 1, node failed; exec=%zu)\n\n", g_exec_count);
    AIRY_FREE(dag_id);
    sched_service_destroy(svc);
    return 0;
}
