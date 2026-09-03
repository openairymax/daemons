// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_dag_core.c
 * @brief DAG 核心语义测试：拓扑执行顺序 / 环检测 / 无效节点 / 优先级队列
 */

#include "test_dag_internal.h"

int test_dag_topological_order(void)
{
    printf("=== test_dag_topological_order ===\n");

    sched_service_t *svc = make_service();
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }
    g_exec_count = 0;
    g_fail_goal = NULL;
    g_block = 0;

    const char *dag_json =
        "{\"name\":\"diamond\",\"nodes\":["
        "{\"id\":\"A\",\"goal\":\"goal-A\",\"role\":\"coding\",\"depends\":[]},"
        "{\"id\":\"B\",\"goal\":\"goal-B\",\"role\":\"coding\",\"depends\":[\"A\"]},"
        "{\"id\":\"C\",\"goal\":\"goal-C\",\"role\":\"coding\",\"depends\":[\"A\"]},"
        "{\"id\":\"D\",\"goal\":\"goal-D\",\"role\":\"coding\",\"depends\":[\"B\",\"C\"]}"
        "]}";

    char *dag_id = NULL;
    int ret = sched_service_submit_dag(svc, dag_json, &dag_id);
    if (ret != AIRY_SUCCESS || !dag_id) {
        printf("  FAILED: submit_dag rc=%d\n", ret);
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

    if (g_exec_count != 4) {
        printf("  FAILED: executor called %zu times (expect 4)\n", g_exec_count);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    if (strstr(g_exec_log[0], "|goal-A") == NULL) {
        printf("  FAILED: first dispatch not A (got: %s)\n", g_exec_log[0]);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    int b_pos = -1, c_pos = -1, d_pos = -1;
    for (size_t i = 0; i < g_exec_count; i++) {
        if (strstr(g_exec_log[i], "|goal-B"))
            b_pos = (int)i;
        if (strstr(g_exec_log[i], "|goal-C"))
            c_pos = (int)i;
        if (strstr(g_exec_log[i], "|goal-D"))
            d_pos = (int)i;
    }
    if (b_pos < 1 || c_pos < 1 || d_pos <= b_pos || d_pos <= c_pos) {
        printf("  FAILED: order B=%d C=%d D=%d (expect B,C>0 and D>B,C)\n", b_pos, c_pos, d_pos);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    printf("  PASSED (exec order: %s | %s | %s | %s)\n\n", g_exec_log[0], g_exec_log[1],
           g_exec_log[2], g_exec_log[3]);
    AIRY_FREE(dag_id);
    sched_service_destroy(svc);
    return 0;
}

int test_dag_cycle_detection(void)
{
    printf("=== test_dag_cycle_detection ===\n");

    sched_service_t *svc = make_service();
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }

    const char *cycle_json = "{\"name\":\"cycle\",\"nodes\":["
                             "{\"id\":\"A\",\"goal\":\"g\",\"depends\":[\"B\"]},"
                             "{\"id\":\"B\",\"goal\":\"g\",\"depends\":[\"A\"]}"
                             "]}";
    char *dag_id = NULL;
    int ret = sched_service_submit_dag(svc, cycle_json, &dag_id);
    if (ret != AIRY_ERR_CYCLE_DETECTED) {
        printf("  FAILED: cycle submit rc=%d (expect %d)\n", ret, AIRY_ERR_CYCLE_DETECTED);
        sched_service_destroy(svc);
        return 1;
    }
    if (dag_id) {
        printf("  FAILED: dag_id should be NULL on cycle\n");
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    printf("  PASSED (cycle rejected)\n\n");
    sched_service_destroy(svc);
    return 0;
}

int test_dag_invalid_node_ids(void)
{
    printf("=== test_dag_invalid_node_ids ===\n");

    sched_service_t *svc = make_service();
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }

    const char *dup_json = "{\"name\":\"dup\",\"nodes\":["
                           "{\"id\":\"A\",\"goal\":\"g1\"},"
                           "{\"id\":\"A\",\"goal\":\"g2\"}"
                           "]}";
    char *dag_id = NULL;
    int ret = sched_service_submit_dag(svc, dup_json, &dag_id);
    if (ret != AIRY_ERR_INVALID_PARAM) {
        printf("  FAILED: duplicate id rc=%d (expect %d)\n", ret, AIRY_ERR_INVALID_PARAM);
        sched_service_destroy(svc);
        return 1;
    }
    if (dag_id) {
        printf("  FAILED: dag_id should be NULL on duplicate id\n");
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    const char *missing_json = "{\"name\":\"missing\",\"nodes\":["
                               "{\"id\":\"A\",\"goal\":\"g\",\"depends\":[\"NOPE\"]}"
                               "]}";
    ret = sched_service_submit_dag(svc, missing_json, &dag_id);
    if (ret != AIRY_ERR_INVALID_PARAM) {
        printf("  FAILED: missing dep rc=%d (expect %d)\n", ret, AIRY_ERR_INVALID_PARAM);
        sched_service_destroy(svc);
        return 1;
    }

    printf("  PASSED (duplicate id & missing dep → INVALID_PARAM)\n\n");
    sched_service_destroy(svc);
    return 0;
}

int test_priority_queue_order(void)
{
    printf("=== test_priority_queue_order ===\n");

    sched_config_t cfg = {.strategy = SCHED_STRATEGY_ROUND_ROBIN,
                          .health_check_interval_ms = 5000,
                          .stats_report_interval_ms = 10000,
                          .enable_ml_strategy = false,
                          .ml_model_path = NULL,
                          .max_agents = 10};
    sched_service_t *svc = NULL;
    if (sched_service_create(&cfg, &svc) != 0) {
        printf("  FAILED: service create\n");
        return 1;
    }
    agent_info_t agent = {.agent_id = "coding",
                          .agent_name = "Coding Agent",
                          .load_factor = 0.2,
                          .success_rate = 0.98,
                          .avg_response_time_ms = 200,
                          .is_available = true,
                          .weight = 1.0};
    sched_service_register_agent(svc, &agent);
    sched_service_set_executor(svc, fake_executor);

    g_exec_count = 0;
    g_fail_goal = NULL;
    g_block = 0;

    sched_task_info_t t = {0};
    char *tid = NULL;
    t.task_description = "goal-L";
    t.priority = TASK_PRIORITY_LOW;
    if (sched_service_submit_task(svc, &t, &tid) != AIRY_SUCCESS || !tid) {
        printf("  FAILED: submit LOW\n");
        sched_service_destroy(svc);
        return 1;
    }
    AIRY_FREE(tid);
    t.task_description = "goal-N";
    t.priority = TASK_PRIORITY_NORMAL;
    if (sched_service_submit_task(svc, &t, &tid) != AIRY_SUCCESS || !tid) {
        printf("  FAILED: submit NORMAL\n");
        sched_service_destroy(svc);
        return 1;
    }
    AIRY_FREE(tid);
    t.task_description = "goal-U";
    t.priority = TASK_PRIORITY_URGENT;
    if (sched_service_submit_task(svc, &t, &tid) != AIRY_SUCCESS || !tid) {
        printf("  FAILED: submit URGENT\n");
        sched_service_destroy(svc);
        return 1;
    }
    AIRY_FREE(tid);

    if (sched_service_start_workers(svc) != AIRY_SUCCESS) {
        printf("  FAILED: start_workers\n");
        sched_service_destroy(svc);
        return 1;
    }

    int waited = 0;
    while (g_exec_count < 3 && waited < 100) {
        TEST_SLEEP_MS(20);
        waited++;
    }
    if (g_exec_count != 3) {
        printf("  FAILED: executor called %zu times (expect 3)\n", g_exec_count);
        sched_service_destroy(svc);
        return 1;
    }
    if (strstr(g_exec_log[0], "|goal-U") == NULL) {
        printf("  FAILED: first dispatch not URGENT (got: %s)\n", g_exec_log[0]);
        sched_service_destroy(svc);
        return 1;
    }
    if (strstr(g_exec_log[1], "|goal-N") == NULL) {
        printf("  FAILED: second dispatch not NORMAL (got: %s)\n", g_exec_log[1]);
        sched_service_destroy(svc);
        return 1;
    }
    if (strstr(g_exec_log[2], "|goal-L") == NULL) {
        printf("  FAILED: third dispatch not LOW (got: %s)\n", g_exec_log[2]);
        sched_service_destroy(svc);
        return 1;
    }

    printf("  PASSED (consume order: %s | %s | %s)\n\n", g_exec_log[0], g_exec_log[1],
           g_exec_log[2]);
    sched_service_destroy(svc);
    return 0;
}

/* 0.1.9 M1-1c：DAG 轻量看板枚举（sched.dag_list 的服务端数据源，
 * CLI /status 与 TUI board 面板经网关消费）。验证空表、参数守卫、
 * 提交两图后 count/id/name/status/progress 字段齐备。 */
int test_dag_list(void)
{
    printf("=== test_dag_list ===\n");

    sched_service_t *svc = make_service();
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }
    g_exec_count = 0;
    g_fail_goal = NULL;
    g_block = 0;

    if (sched_dag_list_json(NULL, NULL) != AIRY_ERR_INVALID_PARAM) {
        printf("  FAILED: NULL params accepted\n");
        sched_service_destroy(svc);
        return 1;
    }

    char *json = NULL;
    if (sched_dag_list_json(svc, &json) != AIRY_SUCCESS || !json) {
        printf("  FAILED: empty list rc/bad json\n");
        sched_service_destroy(svc);
        return 1;
    }
    if (!strstr(json, "\"count\":0") || !strstr(json, "\"dags\":[]")) {
        printf("  FAILED: empty list payload (%s)\n", json);
        AIRY_FREE(json);
        sched_service_destroy(svc);
        return 1;
    }
    AIRY_FREE(json);

    char *id1 = NULL, *id2 = NULL;
    const char *dag1 = "{\"name\":\"l1\",\"nodes\":[{\"id\":\"A\",\"goal\":\"goal-A\"}]}";
    const char *dag2 = "{\"name\":\"l2\",\"nodes\":["
                       "{\"id\":\"B\",\"goal\":\"goal-B\"},"
                       "{\"id\":\"C\",\"goal\":\"goal-C\",\"depends\":[\"B\"]}]}";
    if (sched_service_submit_dag(svc, dag1, &id1) != AIRY_SUCCESS || !id1 ||
        sched_service_submit_dag(svc, dag2, &id2) != AIRY_SUCCESS || !id2) {
        printf("  FAILED: submit_dag\n");
        AIRY_FREE(id1);
        AIRY_FREE(id2);
        sched_service_destroy(svc);
        return 1;
    }
    if (wait_dag_terminal(svc, id1, 5000) != 0 || wait_dag_terminal(svc, id2, 5000) != 0) {
        printf("  FAILED: dag timeout\n");
        AIRY_FREE(id1);
        AIRY_FREE(id2);
        sched_service_destroy(svc);
        return 1;
    }

    if (sched_dag_list_json(svc, &json) != AIRY_SUCCESS || !json) {
        printf("  FAILED: list rc/bad json\n");
        AIRY_FREE(id1);
        AIRY_FREE(id2);
        sched_service_destroy(svc);
        return 1;
    }
    int ok = strstr(json, "\"count\":2") != NULL && strstr(json, id1) != NULL &&
             strstr(json, id2) != NULL && strstr(json, "\"name\":\"l1\"") != NULL &&
             strstr(json, "\"name\":\"l2\"") != NULL &&
             strstr(json, "\"status\":\"completed\"") != NULL &&
             strstr(json, "\"progress\":2") != NULL;
    if (!ok)
        printf("  FAILED: list payload missing fields (%s)\n", json);
    AIRY_FREE(json);
    AIRY_FREE(id1);
    AIRY_FREE(id2);
    sched_service_destroy(svc);
    if (!ok)
        return 1;

    printf("  PASSED (empty list + 2-dag enumeration)\n\n");
    return 0;
}

/* 节点 goal 仅为计划标签（等于节点 id）时，executor 应回退到 DAG 顶层
 * input（原始任务描述），而不是把标签当任务发给 agent。 */
int test_dag_input_fallback(void)
{
    printf("=== test_dag_input_fallback ===\n");

    sched_service_t *svc = make_service();
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }
    g_exec_count = 0;
    g_fail_goal = NULL;
    g_block = 0;

    const char *dag_json =
        "{\"name\":\"input_fallback\",\"input\":\"读取 /etc/hostname\",\"nodes\":["
        "{\"id\":\"reactive_1_step1\",\"goal\":\"reactive_1_step1\",\"role\":\"coding\",\"depends\":[]},"
        "{\"id\":\"reactive_1_step2\",\"goal\":\"\",\"role\":\"coding\",\"depends\":[\"reactive_1_step1\"]}"
        "]}";

    char *dag_id = NULL;
    int ret = sched_service_submit_dag(svc, dag_json, &dag_id);
    if (ret != AIRY_SUCCESS || !dag_id) {
        printf("  FAILED: submit_dag rc=%d\n", ret);
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
    AIRY_FREE(dag_id);
    if (strcmp(st, "completed") != 0) {
        printf("  FAILED: dag status=%s (expect completed)\n", st);
        sched_service_destroy(svc);
        return 1;
    }

    if (g_exec_count != 2) {
        printf("  FAILED: executor called %zu times (expect 2)\n", g_exec_count);
        sched_service_destroy(svc);
        return 1;
    }
    for (size_t i = 0; i < g_exec_count; i++) {
        if (strstr(g_exec_log[i], "|读取 /etc/hostname") == NULL) {
            printf("  FAILED: dispatch[%zu] goal=%s (expect task input fallback)\n", i,
                   g_exec_log[i]);
            sched_service_destroy(svc);
            return 1;
        }
    }

    printf("  PASSED (both nodes dispatched with task input: %s)\n\n", g_exec_log[0]);
    sched_service_destroy(svc);
    return 0;
}
