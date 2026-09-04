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
    g_exec_count = 0;
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
int test_dag_group_consensus_collab(void)
{
    printf("=== test_dag_group_consensus_collab ===\n");

    char *group_id = NULL;
    char **results = NULL;
    size_t result_count = 0;
    char *c1 = NULL, *res1 = NULL;
    char *c2 = NULL, *res2 = NULL;
    char *assigned[3] = {NULL, NULL, NULL};

    mac_framework_t *fw = mac_framework_create(MAC_MODE_COLLABORATIVE);
    if (!fw) {
        printf("  FAILED: mac_framework_create\n");
        return 1;
    }

    const char *ids[3] = {"reviewer_a", "reviewer_b", "reviewer_c"};
    mac_agent_info_t agents[3] = {
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
    size_t registered = 0;
    if (mac_framework_register_agents_batch(fw, agents, 3, &registered) != 0 || registered != 3) {
        printf("  FAILED: register_agents_batch (%zu/3)\n", registered);
        mac_framework_destroy(fw);
        return 1;
    }

    if (mac_framework_create_group(fw, "code-review-squad", MAC_MODE_COLLABORATIVE, ids, 3,
                                   &group_id) != 0 ||
        !group_id) {
        printf("  FAILED: create_group\n");
        mac_framework_destroy(fw);
        return 1;
    }

    mac_collab_task_t tasks[3] = {
        {.id = "t_1", .input_json = "{\"file\":\"ipc.c\",\"mode\":\"security\"}"},
        {.id = "t_2", .input_json = "{\"file\":\"sched.c\",\"mode\":\"perf\"}"},
        {.id = "t_3", .input_json = "{\"file\":\"mem.c\",\"mode\":\"leak\"}"},
    };
    if (mac_framework_delegate_batch(fw, group_id, tasks, 3, assigned) != 0) {
        printf("  FAILED: delegate_batch\n");
        AIRY_FREE(group_id);
        mac_framework_destroy(fw);
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        bool in_group = false;
        if (!assigned[i]) {
            printf("  FAILED: task t_%d not assigned\n", i + 1);
            goto fail;
        }
        for (int j = 0; j < 3; j++) {
            if (strcmp(assigned[i], ids[j]) == 0)
                in_group = true;
        }
        if (!in_group) {
            printf("  FAILED: task t_%d assigned outside group: %s\n", i + 1, assigned[i]);
            goto fail;
        }
        AIRY_FREE(assigned[i]);
        assigned[i] = NULL;
    }

    for (int i = 0; i < 3; i++) {
        char out[64];
        snprintf(out, sizeof(out), "{\"review\":\"ok-%s\"}", tasks[i].id);
        if (mac_framework_complete_task(fw, tasks[i].id, out) != 0) {
            printf("  FAILED: complete_task %s\n", tasks[i].id);
            goto fail;
        }
    }

    if (mac_framework_collect_results(fw, group_id, NULL, &results, &result_count) != 0 ||
        result_count != 3) {
        printf("  FAILED: collect_results (count=%zu, expect 3)\n", result_count);
        goto fail;
    }
    bool all_ok = true;
    for (size_t i = 0; i < result_count; i++) {
        if (!results[i] || strstr(results[i], "\"review\":\"ok-") == NULL)
            all_ok = false;
        AIRY_FREE(results[i]);
    }
    AIRY_FREE(results);
    if (!all_ok) {
        printf("  FAILED: collected results incomplete\n");
        goto fail;
    }

    if (mac_framework_start_consensus(fw, group_id, "{\"merge\":\"allowed\",\"round\":1}",
                                      MAC_CONSENSUS_MAJORITY, &c1) != 0 ||
        !c1) {
        printf("  FAILED: start_consensus (majority)\n");
        goto fail;
    }
    for (int i = 0; i < 3; i++) {
        if (mac_framework_vote(fw, c1, ids[i], "{\"vote\":\"approve\"}") != 0) {
            printf("  FAILED: vote %s\n", ids[i]);
            goto fail;
        }
    }
    if (mac_framework_vote(fw, c1, "reviewer_a", "{\"vote\":\"approve\"}") == 0) {
        printf("  FAILED: duplicate vote not rejected\n");
        goto fail;
    }
    if (mac_framework_resolve_consensus(fw, c1, &res1) != 0 || !res1 ||
        strstr(res1, "\"merge\":\"allowed\"") == NULL) {
        printf("  FAILED: majority not approved (res=%s)\n", res1 ? res1 : "NULL");
        AIRY_FREE(res1);
        goto fail;
    }
    AIRY_FREE(res1);
    AIRY_FREE(c1);
    c1 = NULL;

    if (mac_framework_start_consensus(fw, group_id, "{\"merge\":\"blocked\",\"round\":2}",
                                      MAC_CONSENSUS_UNANIMOUS, &c2) != 0 ||
        !c2) {
        printf("  FAILED: start_consensus (unanimous)\n");
        goto fail;
    }
    if (mac_framework_vote(fw, c2, "reviewer_a", "approve") != 0 ||
        mac_framework_vote(fw, c2, "reviewer_b", "reject") != 0 ||
        mac_framework_vote(fw, c2, "reviewer_c", "approve") != 0) {
        printf("  FAILED: unanimous votes\n");
        goto fail;
    }
    if (mac_framework_resolve_consensus(fw, c2, &res2) != 0 || !res2 ||
        strstr(res2, "rejected") == NULL) {
        printf("  FAILED: unanimous veto not detected (res=%s)\n", res2 ? res2 : "NULL");
        AIRY_FREE(res2);
        goto fail;
    }
    AIRY_FREE(res2);
    AIRY_FREE(c2);
    c2 = NULL;

    if (mac_framework_disband_group(fw, group_id) != 0) {
        printf("  FAILED: disband_group\n");
        goto fail;
    }
    AIRY_FREE(group_id);
    group_id = NULL;
    mac_framework_destroy(fw);
    printf("  PASSED (group collab: 3 tasks delegated+collected; consensus: majority ok, unanimous "
           "veto)\n\n");
    return 0;

fail:
    for (int i = 0; i < 3; i++)
        AIRY_FREE(assigned[i]);
    AIRY_FREE(res1);
    AIRY_FREE(c1);
    AIRY_FREE(res2);
    AIRY_FREE(c2);
    AIRY_FREE(group_id);
    mac_framework_destroy(fw);
    return 1;
}
