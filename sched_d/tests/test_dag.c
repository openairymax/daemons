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
#include "multi_agent_collaboration.h"

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

static char g_exec_log[64][256];
static size_t g_exec_count;
static const char *g_fail_goal;
static const char *g_fatal_goal;
static const char *g_flaky_goal;
static int g_flaky_left;
static volatile int g_block;

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
    if (g_fatal_goal && task_description && strstr(task_description, g_fatal_goal)) {
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    if (g_fail_goal && task_description && strstr(task_description, g_fail_goal)) {
        return AIRY_ERR_EXEC_FAIL;
    }
    if (g_flaky_goal && task_description && strstr(task_description, g_flaky_goal)) {
        if (g_flaky_left > 0) {
            g_flaky_left--;
            return AIRY_ERR_EXEC_TIMEOUT;
        }
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

/* 分级语义服务工厂（改进3/4）：显式配置 dag_fatal_cascade 与并行度。
 * fatal_cascade=true → 仅 FATAL 级联整图（生产默认）；false → 旧行为。
 * parallel>0 → mac 委派批次派发（注册 3 个 agent 供选人）。 */
static sched_service_t *make_service_graded(bool fatal_cascade, uint32_t parallel)
{
    sched_config_t cfg = {.strategy = SCHED_STRATEGY_ROUND_ROBIN,
                          .health_check_interval_ms = 5000,
                          .stats_report_interval_ms = 10000,
                          .enable_ml_strategy = false,
                          .ml_model_path = NULL,
                          .max_agents = 10,
                          .dag_max_parallel = parallel,
                          .dag_batch_size = parallel,
                          .dag_fatal_cascade = fatal_cascade};
    sched_service_t *svc = NULL;
    if (sched_service_create(&cfg, &svc) != 0)
        return NULL;
    for (int i = 0; i < 3; i++) {
        char id[32], name[64];
        snprintf(id, sizeof(id), "agent_%d", i);
        snprintf(name, sizeof(name), "Agent %d", i);
        agent_info_t agent = {.agent_id = id,
                              .agent_name = name,
                              .load_factor = 0.2,
                              .success_rate = 0.98,
                              .avg_response_time_ms = 200,
                              .is_available = true,
                              .weight = 1.0};
        sched_service_register_agent(svc, &agent);
    }
    sched_service_set_executor(svc, fake_executor);
    sched_service_start_workers(svc);
    return svc;
}

/* 并行模式服务：dag_max_parallel>0 启用 mac_framework 委派批次派发。
 * 注册 3 个 agent（同权重），mac select 按性能分选人并做并发限流。 */
static sched_service_t *make_parallel_service(void)
{
    sched_config_t cfg = {.strategy = SCHED_STRATEGY_ROUND_ROBIN,
                          .health_check_interval_ms = 5000,
                          .stats_report_interval_ms = 10000,
                          .enable_ml_strategy = false,
                          .ml_model_path = NULL,
                          .max_agents = 10,
                          .dag_max_parallel = 4,
                          .dag_batch_size = 4};
    sched_service_t *svc = NULL;
    if (sched_service_create(&cfg, &svc) != 0)
        return NULL;

    for (int i = 0; i < 3; i++) {
        char id[32], name[64];
        snprintf(id, sizeof(id), "agent_%d", i);
        snprintf(name, sizeof(name), "Agent %d", i);
        agent_info_t agent = {.agent_id = id,
                              .agent_name = name,
                              .load_factor = 0.2,
                              .success_rate = 0.98,
                              .avg_response_time_ms = 200,
                              .is_available = true,
                              .weight = 1.0};
        sched_service_register_agent(svc, &agent);
    }
    sched_service_set_executor(svc, fake_executor);
    sched_service_start_workers(svc);
    return svc;
}

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
    return -1;
}

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

static int test_dag_cycle_detection(void)
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

static int test_dag_invalid_node_ids(void)
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

    printf("  PASSED (consume order: %s | %s | %s)\n\n", g_exec_log[0], g_exec_log[1],
           g_exec_log[2]);
    sched_service_destroy(svc);
    return 0;
}

/* ---- 用例 7：并行委派模式（dag_max_parallel>0，mac_framework 接线） ----
 * 菱形 DAG：A → B/C（并行层）→ D。验证：
 *  1) 并行路径正确完成（拓扑顺序不破坏）
 *  2) 并发执行真实发生：executor 记录同时在执行的最大数 ≥ 2
 *     （B/C 同层无依赖，批次收集后经线程池并发执行）
 */
static int g_concurrent_now;
static int g_concurrent_max;

static int parallel_executor(const char *agent_id, const char *task_description, char **out_output)
{
    exec_log_push(agent_id ? agent_id : "?", task_description ? task_description : "");
    int now = ++g_concurrent_now;
    if (now > g_concurrent_max)
        g_concurrent_max = now;
    TEST_SLEEP_MS(30);
    --g_concurrent_now;
    char buf[128];
    snprintf(buf, sizeof(buf), "p[%s:%s]", agent_id ? agent_id : "?",
             task_description ? task_description : "");
    *out_output = AIRY_STRDUP(buf);
    return AIRY_SUCCESS;
}

static int test_dag_parallel_delegation(void)
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

/* ---- 用例 8：委派模式 + group 协作 + consensus 机制（mac_framework 直接验证） ----
 * 场景：3 名评审 agent 组成代码评审小组（COLLABORATIVE），批量委派 3 个评审
 * 任务（delegate_batch）→ 回写（complete_task）→ 收集（collect_results），
 * 再经 consensus 仲裁（MAJORITY 多数通过 / UNANIMOUS 一票否决）形成组决策。
 * 验证：
 *  1) group 成员约束：委派全部落到组成员
 *  2) delegate_batch 批量委派 + complete_task 回写闭环
 *  3) collect_results 收集全部完成结果
 *  4) consensus 多数/全票策略判定正确（含一票否决）
 *  5) 重复投票被拒（同一 agent 仅一票）
 */
static int test_dag_group_consensus_collab(void)
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

/* ---- 用例 9：普通失败不级联整图（改进3，dag_fatal_cascade=true 生产默认） ----
 * B 普通失败 → 仅取消依赖它的不可达下游 C；独立分支 A 正常完成；
 * 图最终如实收敛 FAILED（部分失败）。 */
static int test_dag_normal_failure_no_cascade(void)
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

/* ---- 用例 10：FATAL 失败级联取消整图（改进3，fail-closed） ----
 * 并行批次中 B 触发 FATAL（OOM）→ 整图 FAILED，未完成节点全部取消，
 * 独立分支 A 的结果也被丢弃（图已收敛）。 */
static int test_dag_fatal_cascade_whole(void)
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
    if (g_exec_count != 2) {
        printf("  FAILED: executor called %zu times (expect 2: A+B, C canceled)\n", g_exec_count);
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

/* ---- 用例 11：transient 失败分级重试成功（改进4） ----
 * A 前 2 次 transient 超时 → 指数退避重试 → 第 3 次成功；
 * 图 COMPLETED，节点 retry_count==2。 */
static int test_dag_transient_retry(void)
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

/* ---- 用例 12：transient 重试耗尽 → 普通失败（改进4） ----
 * A 一直 transient 失败，max_retries=1 → 重试 1 次后 FAILED，图 FAILED。 */
static int test_dag_transient_retry_exhausted(void)
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

int main(void)
{
    int failed = 0;
    failed += test_dag_topological_order();
    failed += test_dag_cycle_detection();
    failed += test_dag_failure_cascade();
    failed += test_dag_cancel();
    failed += test_dag_invalid_node_ids();
    failed += test_priority_queue_order();
    failed += test_dag_parallel_delegation();
    failed += test_dag_group_consensus_collab();

    failed += test_dag_normal_failure_no_cascade();
    failed += test_dag_fatal_cascade_whole();
    failed += test_dag_transient_retry();
    failed += test_dag_transient_retry_exhausted();

    if (failed) {
        printf("\nDAG tests: %d FAILED\n", failed);
        return 1;
    }
    printf("\nDAG tests: ALL PASSED\n");
    return 0;
}
