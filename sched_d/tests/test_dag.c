// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file test_dag.c
 * @brief DAG 任务图执行引擎单元测试（工作大厅机制）
 *
 * 覆盖：拓扑执行顺序 / 依赖就绪 / 环检测拒绝 / 失败级联取消 /
 *       用户取消 / 状态看板查询。注入假 executor 记录派发顺序，
 * 不依赖真实 agent_d（与 test_scheduler.c 同构）。
 */

#include "scheduler_service.h"
#include "airy_memory.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define TEST_SLEEP_MS(ms) Sleep((ms))
#else
#include <unistd.h>
#define TEST_SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* ---- 假 executor：记录 (role, goal) 派发日志；支持按 goal 注入失败/阻塞 ---- */

static char g_exec_log[64][256];
static size_t g_exec_count;
static const char *g_fail_goal;       /* goal 含此串 → 返回失败（NULL 不注入） */
static volatile int g_block;          /* 置 1 时 executor 阻塞（取消测试用） */

static void exec_log_push(const char *role, const char *goal)
{
    if (g_exec_count < 64) {
        snprintf(g_exec_log[g_exec_count], sizeof(g_exec_log[0]), "%s|%s", role, goal);
        g_exec_count++;
    }
}

static int fake_executor(const char *agent_id, const char *task_description, char **out_output)
{
    exec_log_push(agent_id ? agent_id : "?", task_description ? task_description : "");

    if (g_block) {
        while (g_block)
            TEST_SLEEP_MS(20);
    }
    if (g_fail_goal && task_description && strstr(task_description, g_fail_goal)) {
        return AIRY_ERR_EXEC_FAIL;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "done[%s]", task_description ? task_description : "");
    *out_output = AIRY_STRDUP(buf);
    return AIRY_SUCCESS;
}

static sched_service_t *make_service(void)
{
    sched_config_t cfg = {.strategy = SCHED_STRATEGY_ROUND_ROBIN,
                          .health_check_interval_ms = 5000,
                          .stats_report_interval_ms = 10000,
                          .enable_ml_strategy = false,
                          .ml_model_path = NULL,
                          .max_agents = 10};
    sched_service_t *svc = NULL;
    if (sched_service_create(&cfg, &svc) != 0)
        return NULL;

    agent_info_t agent = {.agent_id = "coding",
                          .agent_name = "Coding Agent",
                          .load_factor = 0.2,
                          .success_rate = 0.98,
                          .avg_response_time_ms = 200,
                          .is_available = true,
                          .weight = 1.0};
    sched_service_register_agent(svc, &agent);
    sched_service_set_executor(svc, fake_executor);
    sched_service_start_workers(svc);
    return svc;
}

/* 轮询 dag_status 直到非 active（带超时） */
static int wait_dag_terminal(sched_service_t *svc, const char *dag_id, int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        char *json = NULL;
        if (sched_service_get_dag(svc, dag_id, &json) == AIRY_SUCCESS && json) {
            int terminal = (strstr(json, "\"status\":\"active\"") == NULL);
            AIRY_FREE(json);
            if (terminal)
                return 0;
        } else if (json) {
            AIRY_FREE(json);
        }
        TEST_SLEEP_MS(50);
        elapsed += 50;
    }
    return -1; /* 超时 */
}

/* 解析 dag_status 图状态字符串：找到 "status":"xxx"（图的，第一个） */
static int get_dag_status_str(sched_service_t *svc, const char *dag_id, char *out, size_t cap)
{
    char *json = NULL;
    if (sched_service_get_dag(svc, dag_id, &json) != AIRY_SUCCESS || !json)
        return -1;
    const char *p = strstr(json, "\"status\":\"");
    if (!p) {
        AIRY_FREE(json);
        return -1;
    }
    p += strlen("\"status\":\"");
    const char *end = strchr(p, '"');
    if (!end) {
        AIRY_FREE(json);
        return -1;
    }
    size_t len = (size_t)(end - p);
    if (len >= cap)
        len = cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    AIRY_FREE(json);
    return 0;
}

/* ---- 用例 1：菱形 DAG 拓扑执行（A→B/C→D），顺序与状态断言 ---- */
static int test_dag_topological_order(void)
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

    /* 顺序断言：A 最先；B/C 在 A 后；D 最后 */
    if (strstr(g_exec_log[0], "|goal-A") == NULL) {
        printf("  FAILED: first dispatch not A (got: %s)\n", g_exec_log[0]);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    int b_pos = -1, c_pos = -1, d_pos = -1;
    for (size_t i = 0; i < g_exec_count; i++) {
        if (strstr(g_exec_log[i], "|goal-B")) b_pos = (int)i;
        if (strstr(g_exec_log[i], "|goal-C")) c_pos = (int)i;
        if (strstr(g_exec_log[i], "|goal-D")) d_pos = (int)i;
    }
    if (b_pos < 1 || c_pos < 1 || d_pos <= b_pos || d_pos <= c_pos) {
        printf("  FAILED: order B=%d C=%d D=%d (expect B,C>0 and D>B,C)\n", b_pos, c_pos, d_pos);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    printf("  PASSED (exec order: %s | %s | %s | %s)\n\n",
           g_exec_log[0], g_exec_log[1], g_exec_log[2], g_exec_log[3]);
    AIRY_FREE(dag_id);
    sched_service_destroy(svc);
    return 0;
}

/* ---- 用例 2：环检测 ---- */
static int test_dag_cycle_detection(void)
{
    printf("=== test_dag_cycle_detection ===\n");

    sched_service_t *svc = make_service();
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }

    const char *cycle_json =
        "{\"name\":\"cycle\",\"nodes\":["
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

/* ---- 用例 3：节点失败 → 图 failed + 依赖级联取消 ---- */
static int test_dag_failure_cascade(void)
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
    g_fail_goal = "boom-B"; /* B 的 goal 命中 → 返回失败 */

    const char *dag_json =
        "{\"name\":\"fail\",\"nodes\":["
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
    /* B 失败后 C 不得执行（级联取消） */
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

/* ---- 用例 4：用户取消 DAG ---- */
static int test_dag_cancel(void)
{
    printf("=== test_dag_cancel ===\n");

    sched_service_t *svc = make_service();
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }
    g_exec_count = 0;
    g_fail_goal = NULL;
    g_block = 1; /* 首节点执行中阻塞，模拟长任务 */

    const char *dag_json =
        "{\"name\":\"cancel\",\"nodes\":["
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

    TEST_SLEEP_MS(200); /* 让 A 进入 running（executor 阻塞） */
    ret = sched_service_cancel_dag(svc, dag_id);
    if (ret != AIRY_SUCCESS) {
        printf("  FAILED: cancel_dag rc=%d\n", ret);
        g_block = 0;
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }
    g_block = 0; /* 释放 executor */

    char st[32];
    get_dag_status_str(svc, dag_id, st, sizeof(st));
    if (strcmp(st, "canceled") != 0) {
        printf("  FAILED: dag status=%s (expect canceled)\n", st);
        AIRY_FREE(dag_id);
        sched_service_destroy(svc);
        return 1;
    }

    /* P0-1 验证：cancel 时 RUNNING 节点（A）完成后必须归一为 canceled，
     * 且输出被丢弃（头文件承诺「不再上屏输出」）。 */
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

/* ---- 用例 5：结构非法错误码区分（重复 id / 依赖缺失 → INVALID_PARAM，非环） ---- */
static int test_dag_invalid_node_ids(void)
{
    printf("=== test_dag_invalid_node_ids ===\n");

    sched_service_t *svc = make_service();
    if (!svc) {
        printf("  FAILED: service create\n");
        return 1;
    }

    /* 5a：重复 id → INVALID_PARAM */
    const char *dup_json =
        "{\"name\":\"dup\",\"nodes\":["
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

    /* 5b：依赖指向不存在的节点 → INVALID_PARAM */
    const char *missing_json =
        "{\"name\":\"missing\",\"nodes\":["
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

/* ---- 用例 6：优先级队列消费顺序（URGENT 优先，同优先级 FIFO） ---- */
static int test_priority_queue_order(void)
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

    /* 先入队（worker 未启动，无消费者），再启动 worker 验证优先级消费 */
    task_info_t t = {0};
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

    printf("  PASSED (consume order: %s | %s | %s)\n\n",
           g_exec_log[0], g_exec_log[1], g_exec_log[2]);
    sched_service_destroy(svc);
    return 0;
}

int main(void)
{
    int failed = 0;
    failed += test_dag_topological_order();
    failed += test_dag_cycle_detection();
    failed += test_dag_failure_cascade();
    failed += test_dag_cancel();
    failed += test_dag_invalid_node_ids();
    failed += test_priority_queue_order();

    if (failed) {
        printf("\nDAG tests: %d FAILED\n", failed);
        return 1;
    }
    printf("\nDAG tests: ALL PASSED\n");
    return 0;
}
