// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_dag_parallel.c
 * @brief DAG 并行委派与多智能体共识协作测试
 */

#include "test_dag_internal.h"

/* ---- Case 7: parallel delegation mode (dag_max_parallel>0, mac_framework
 * wiring) ----
 * Diamond DAG: A -> B/C (parallel layer) -> D. Verifies:
 *  1) the parallel path completes correctly (topological order preserved)
 *  2) concurrency really happens: the executor records max concurrent >= 2
 *     (B/C are same-layer, dependency-free; after batch collection they run
 *     concurrently via the thread pool)
 */
int test_dag_parallel_delegation(void)
{
    printf("=== test_dag_parallel_delegation ===\n");

    sched_service_t *svc = make_parallel_service();
    if (!svc) {
        printf("  FAILED: parallel service create\n");
        return 1;
    }
    exec_log_reset();
    g_concurrent_now = 0;
    g_concurrent_max = 0;
    g_fail_goal = NULL;
    g_block = 0;
    sched_service_set_executor(svc, parallel_executor);

    const char *dag_json = "{\"name\":\"para\",\"nodes\":["
                           "{\"id\":\"A\",\"goal\":\"goal-A\",\"depends\":[]},"
                           "{\"id\":\"B\",\"goal\":\"goal-B\",\"depends\":[\"A\"]},"
                           "{\"id\":\"C\",\"goal\":\"goal-C\",\"depends\":[\"A\"]},"
                           "{\"id\":\"D\",\"goal\":\"goal-D\",\"depends\":[\"B\",\"C\"]}"
                           "]}";

    char *dag_id = NULL;
    int ret = sched_service_submit_dag(svc, dag_json, &dag_id);
    if (ret != AIRY_SUCCESS || !dag_id) {
        printf("  FAILED: submit_dag rc=%d\n", ret);
        sched_service_destroy(svc);
        return 1;
    }

    /* macOS CI 实证（2026-09-04）：并行委派首节点完成事件存在 ~10s 级延迟
     *（疑 thread pool/mac 唤醒机制，待专项根因），5s 上限在 mac 上必超时。
     * 放宽为 30s 只放宽等待护栏，仍校验 completed 状态与并发真实性
     *（g_concurrent_max>=2）；10s 唤醒延迟的机制问题另立专项排查。 */
    if (wait_dag_terminal(svc, dag_id, 30000) != 0) {
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
    if (g_exec_count != 4) {
        printf("  FAILED: executor called %zu times (expect 4)\n", g_exec_count);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    if (g_concurrent_max < 2) {
        printf("  FAILED: no real concurrency (max concurrent=%d, expect ≥2)\n", g_concurrent_max);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    printf("  PASSED (max concurrent=%d, order preserved)\n", g_concurrent_max);
    AIRY_FREE(dag_id);
    sched_service_destroy(svc);
    return 0;
}

/* ---- Case 9: upstream product injection on the parallel batch path
 * (0.1.17 R2-B) ----
 * Two dependency-free roots (A, B) feeding one sink (C): the batch path collects
 * {A, B} as the first parallel layer and C as the second, so C must receive both
 * direct upstream products in its agent input. Covers the mac delegate_batch
 * branch of sched_dag_worker.c, whose input composition is separate from the
 * serial one exercised by test_dag_upstream_inject().
 */
int test_dag_upstream_inject_par(void)
{
    printf("=== test_dag_upstream_inject_par ===\n");

    sched_service_t *svc = make_parallel_service();
    if (!svc) {
        printf("  FAILED: parallel service create\n");
        return 1;
    }
    exec_log_reset();
    g_concurrent_now = 0;
    g_concurrent_max = 0;
    g_fail_goal = NULL;
    g_fatal_goal = NULL;
    g_flaky_goal = NULL;
    g_block = 0;
    sched_service_set_executor(svc, parallel_executor);

    const char *dag_json = "{\"name\":\"para_inject\",\"nodes\":["
                           "{\"id\":\"A\",\"goal\":\"goal-A\",\"depends\":[]},"
                           "{\"id\":\"B\",\"goal\":\"goal-B\",\"depends\":[]},"
                           "{\"id\":\"C\",\"goal\":\"goal-C\",\"depends\":[\"A\",\"B\"]}"
                           "]}";

    char *dag_id = NULL;
    if (sched_service_submit_dag(svc, dag_json, &dag_id) != AIRY_SUCCESS || !dag_id) {
        printf("  FAILED: submit_dag\n");
        sched_service_destroy(svc);
        return 1;
    }
    if (wait_dag_terminal(svc, dag_id, 30000) != 0) {
        printf("  FAILED: dag timeout\n");
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    char st[32];
    get_dag_status_str(svc, dag_id, st, sizeof(st));
    AIRY_FREE(dag_id);
    if (strcmp(st, "completed") != 0) {
        printf("  FAILED: dag status=%s (expect completed)\n", st);
        sched_service_destroy(svc);
        return 1;
    }
    if (g_exec_count != 3) {
        printf("  FAILED: executor called %zu times (expect 3)\n", g_exec_count);
        sched_service_destroy(svc);
        return 1;
    }

    const char *c_log = NULL;
    for (size_t i = 0; i < g_exec_count; i++) {
        /* 根节点不得被注入上游段 */
        if (strstr(g_exec_log[i], "|goal-A") || strstr(g_exec_log[i], "|goal-B")) {
            if (strstr(g_exec_log[i], "上游产出")) {
                printf("  FAILED: root node got downstream injection: %s\n", g_exec_log[i]);
                sched_service_destroy(svc);
                return 1;
            }
        }
        if (strstr(g_exec_log[i], "|goal-C"))
            c_log = g_exec_log[i];
    }
    if (!c_log) {
        printf("  FAILED: sink node C never dispatched\n");
        sched_service_destroy(svc);
        return 1;
    }
    /* C 的输入 = 自身 goal + A/B 两个上游产出（parallel_executor 产出 p[..:goal-X]） */
    if (strstr(c_log, "上游产出 [A]") == NULL || strstr(c_log, "上游产出 [B]") == NULL ||
        strstr(c_log, ":goal-A]") == NULL || strstr(c_log, ":goal-B]") == NULL) {
        printf("  FAILED: sink input=%s\n", c_log);
        sched_service_destroy(svc);
        return 1;
    }

    printf("  PASSED (sink input carries both upstream products)\n\n");
    sched_service_destroy(svc);
    return 0;
}

/* ---- Case 8: delegation mode + group collaboration + consensus mechanism
 * (mac_framework direct verification) ----
 * Scenario: 3 review agents form a code-review group (COLLABORATIVE); 3 review
 * tasks are batch-delegated (delegate_batch) -> written back (complete_task)
 * -> collected (collect_results), then arbitrated via consensus (MAJORITY
 * passes / UNANIMOUS veto) into a group decision.
 * Verifies:
 *  1) group member constraint: all delegations land on group members
 *  2) delegate_batch batch delegation + complete_task write-back closed loop
 *  3) collect_results collects all completed results
 *  4) consensus majority/unanimity strategies decide correctly (incl. veto)
 *  5) duplicate voting rejected (one vote per agent)
 */
/* 三名评审的静态档案表：成员 id 同时用于建组名单、委派落点校验与投票，
 * 单一来源避免三处字符串漂移。 */
static const mac_agent_info_t k_reviewers[3] = {
    {.id = "reviewer_a",
     .name = "Reviewer A",
     .performance_score = 0.9,
     .reliability_score = 0.8,
     .max_concurrent_tasks = 3,
     .available = true},
    {.id = "reviewer_b",
     .name = "Reviewer B",
     .performance_score = 0.8,
     .reliability_score = 0.9,
     .max_concurrent_tasks = 3,
     .available = true},
    {.id = "reviewer_c",
     .name = "Reviewer C",
     .performance_score = 0.7,
     .reliability_score = 0.7,
     .max_concurrent_tasks = 3,
     .available = true},
};

/* 三项评审任务：委派与回写共用同一份任务表。 */
static const mac_collab_task_t k_tasks[3] = {
    {.id = "t_1", .input_json = "{\"file\":\"ipc.c\",\"mode\":\"security\"}"},
    {.id = "t_2", .input_json = "{\"file\":\"sched.c\",\"mode\":\"perf\"}"},
    {.id = "t_3", .input_json = "{\"file\":\"mem.c\",\"mode\":\"leak\"}"},
};

/* 注册三名评审并组建协作组。 */
static int squad_setup(mac_framework_t *fw, char **group_id)
{
    const char *ids[3] = {k_reviewers[0].id, k_reviewers[1].id, k_reviewers[2].id};
    size_t registered = 0;

    if (mac_framework_register_agents_batch(fw, k_reviewers, 3, &registered) != 0 ||
        registered != 3) {
        printf("  FAILED: register_agents_batch (%zu/3)\n", registered);
        return 1;
    }
    if (mac_framework_create_group(fw, "code-review-squad", MAC_MODE_COLLABORATIVE, ids, 3,
                                   group_id) != 0 ||
        !*group_id) {
        printf("  FAILED: create_group\n");
        return 1;
    }
    return 0;
}

/* 批量委派三项评审，并校验每个落点都在组成员内。 */
static int delegate_all(mac_framework_t *fw, const char *group_id, char **assigned)
{
    if (mac_framework_delegate_batch(fw, group_id, k_tasks, 3, assigned) != 0) {
        printf("  FAILED: delegate_batch\n");
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        bool in_group = false;

        if (!assigned[i]) {
            printf("  FAILED: task t_%d not assigned\n", i + 1);
            return 1;
        }
        for (int j = 0; j < 3; j++) {
            if (strcmp(assigned[i], k_reviewers[j].id) == 0)
                in_group = true;
        }
        if (!in_group) {
            printf("  FAILED: task t_%d assigned outside group: %s\n", i + 1, assigned[i]);
            return 1;
        }
        AIRY_FREE(assigned[i]);
        assigned[i] = NULL;
    }
    return 0;
}

/* 回写三项评审产出，闭合 delegate → complete → collect 环。 */
static int complete_all(mac_framework_t *fw)
{
    for (int i = 0; i < 3; i++) {
        char out[64];

        snprintf(out, sizeof(out), "{\"review\":\"ok-%s\"}", k_tasks[i].id);
        if (mac_framework_complete_task(fw, k_tasks[i].id, out) != 0) {
            printf("  FAILED: complete_task %s\n", k_tasks[i].id);
            return 1;
        }
    }
    return 0;
}

/* 收集全部完成结果，校验每项产出都带 review 标记。 */
static int collect_all(mac_framework_t *fw, const char *group_id)
{
    char **results = NULL;
    size_t result_count = 0;
    bool all_ok = true;

    if (mac_framework_collect_results(fw, group_id, NULL, &results, &result_count) != 0 ||
        result_count != 3) {
        printf("  FAILED: collect_results (count=%zu, expect 3)\n", result_count);
        return 1;
    }
    for (size_t i = 0; i < result_count; i++) {
        if (!results[i] || strstr(results[i], "\"review\":\"ok-") == NULL)
            all_ok = false;
        AIRY_FREE(results[i]);
    }
    AIRY_FREE(results);
    if (!all_ok) {
        printf("  FAILED: collected results incomplete\n");
        return 1;
    }
    return 0;
}

/* 多数决：全票赞成通过，且同一 agent 重复投票必须被拒。 */
static int cons_majority(mac_framework_t *fw, const char *group_id)
{
    char *cid = NULL;
    char *res = NULL;
    int rc = 1;

    if (mac_framework_start_consensus(fw, group_id, "{\"merge\":\"allowed\",\"round\":1}",
                                      MAC_CONSENSUS_MAJORITY, &cid) != 0 ||
        !cid) {
        printf("  FAILED: start_consensus (majority)\n");
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        if (mac_framework_vote(fw, cid, k_reviewers[i].id, "{\"vote\":\"approve\"}") != 0) {
            printf("  FAILED: vote %s\n", k_reviewers[i].id);
            goto out;
        }
    }
    if (mac_framework_vote(fw, cid, "reviewer_a", "{\"vote\":\"approve\"}") == 0) {
        printf("  FAILED: duplicate vote not rejected\n");
        goto out;
    }
    if (mac_framework_resolve_consensus(fw, cid, &res) != 0 || !res ||
        strstr(res, "\"merge\":\"allowed\"") == NULL) {
        printf("  FAILED: majority not approved (res=%s)\n", res ? res : "NULL");
        goto out;
    }
    rc = 0;
out:
    AIRY_FREE(res);
    AIRY_FREE(cid);
    return rc;
}

/* 一致决：任一否决即整体否决（veto）。 */
static int cons_unanimous(mac_framework_t *fw, const char *group_id)
{
    char *cid = NULL;
    char *res = NULL;
    int rc = 1;

    if (mac_framework_start_consensus(fw, group_id, "{\"merge\":\"blocked\",\"round\":2}",
                                      MAC_CONSENSUS_UNANIMOUS, &cid) != 0 ||
        !cid) {
        printf("  FAILED: start_consensus (unanimous)\n");
        return 1;
    }
    if (mac_framework_vote(fw, cid, "reviewer_a", "approve") != 0 ||
        mac_framework_vote(fw, cid, "reviewer_b", "reject") != 0 ||
        mac_framework_vote(fw, cid, "reviewer_c", "approve") != 0) {
        printf("  FAILED: unanimous votes\n");
        goto out;
    }
    if (mac_framework_resolve_consensus(fw, cid, &res) != 0 || !res ||
        strstr(res, "rejected") == NULL) {
        printf("  FAILED: unanimous veto not detected (res=%s)\n", res ? res : "NULL");
        goto out;
    }
    rc = 0;
out:
    AIRY_FREE(res);
    AIRY_FREE(cid);
    return rc;
}

int test_dag_group_consensus_collab(void)
{
    char *assigned[3] = {NULL, NULL, NULL};
    char *group_id = NULL;
    mac_framework_t *fw;
    int rc = 1;

    printf("=== test_dag_group_consensus_collab ===\n");

    fw = mac_framework_create(MAC_MODE_COLLABORATIVE);
    if (!fw) {
        printf("  FAILED: mac_framework_create\n");
        return 1;
    }
    if (squad_setup(fw, &group_id) != 0)
        goto out;
    if (delegate_all(fw, group_id, assigned) != 0)
        goto out;
    if (complete_all(fw) != 0)
        goto out;
    if (collect_all(fw, group_id) != 0)
        goto out;
    if (cons_majority(fw, group_id) != 0)
        goto out;
    if (cons_unanimous(fw, group_id) != 0)
        goto out;
    if (mac_framework_disband_group(fw, group_id) != 0) {
        printf("  FAILED: disband_group\n");
        goto out;
    }
    printf("  PASSED (group collab: 3 tasks delegated+collected; consensus: majority ok, unanimous "
           "veto)\n\n");
    rc = 0;
out:
    for (int i = 0; i < 3; i++)
        AIRY_FREE(assigned[i]);
    AIRY_FREE(group_id);
    mac_framework_destroy(fw);
    return rc;
}
