// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_dag.c
 * @brief DAG 任务图执行引擎单元测试主文件（共享状态/辅助函数/main）
 *
 * 覆盖：拓扑执行顺序 / 依赖就绪 / 环检测拒绝 / 失败级联取消 /
 *       用户取消 / 状态看板查询。注入假 executor 记录派发顺序，
 *       不依赖真实 agent_d（与 test_scheduler.c 同构）。
 * 各测试函数拆分至 test_dag_core.c / test_dag_failure.c /
 * test_dag_parallel.c，经 test_dag_internal.h 声明后由 main() 调用。
 */

#include "test_dag_internal.h"
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
#ifndef TEST_SLEEP_MS
#define TEST_SLEEP_MS(ms) Sleep((ms))
#endif
#else
#include <unistd.h>
#ifndef TEST_SLEEP_MS
#define TEST_SLEEP_MS(ms) usleep((ms) * 1000)
#endif
#endif

/* ---- 共享全局测试状态（非 static：各域文件经 test_dag_internal.h 访问） ---- */
char g_exec_log[64][256];
size_t g_exec_count;
const char *g_fail_goal;
const char *g_fatal_goal;
const char *g_flaky_goal;
int g_flaky_left;
volatile int g_block;
int g_concurrent_now;
int g_concurrent_max;

void exec_log_push(const char *role, const char *goal)
{
    if (g_exec_count < 64) {
        snprintf(g_exec_log[g_exec_count], sizeof(g_exec_log[0]), "%s|%s", role, goal);
        g_exec_count++;
    }
}

int fake_executor(const char *agent_id, const char *task_description, const char *workspace_dir,
                  char **out_output)
{
    (void)workspace_dir;
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

sched_service_t *make_service(void)
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

/* Graded-semantics service factory (improvements 3/4): explicitly configure
 * dag_fatal_cascade and parallelism.
 * fatal_cascade=true -> only FATAL cascades the whole graph (production
 * default); false -> legacy behavior.
 * parallel>0 -> mac delegated batch dispatch (registers 3 agents for
 * selection). */
sched_service_t *make_service_graded(bool fatal_cascade, uint32_t parallel)
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

/* Parallel-mode service: dag_max_parallel>0 enables mac_framework delegated
 * batch dispatch. Registers 3 agents (equal weight); mac select picks by
 * performance score and applies concurrency throttling. */
sched_service_t *make_parallel_service(void)
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

int wait_dag_terminal(sched_service_t *svc, const char *dag_id, int timeout_ms)
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

int get_dag_status_str(sched_service_t *svc, const char *dag_id, char *out, size_t cap)
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
    AIRY_MEMCPY(out, p, len);
    out[len] = '\0';
    AIRY_FREE(json);
    return 0;
}

/* ---- Case 7: parallel delegation mode (dag_max_parallel>0, mac_framework
 * wiring) ----
 * Diamond DAG: A -> B/C (parallel layer) -> D. Verifies:
 *  1) the parallel path completes correctly (topological order preserved)
 *  2) concurrency really happens: the executor records max concurrent >= 2
 *     (B/C are same-layer, dependency-free; after batch collection they run
 *     concurrently via the thread pool)
 */
int parallel_executor(const char *agent_id, const char *task_description,
                      const char *workspace_dir, char **out_output)
{
    (void)workspace_dir;
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

int main(void)
{
    int failed = 0;
    failed += test_dag_topological_order();
    failed += test_dag_cycle_detection();
    failed += test_dag_failure_cascade();
    failed += test_dag_cancel();
    failed += test_dag_invalid_node_ids();
    failed += test_priority_queue_order();
    failed += test_dag_input_fallback();
    failed += test_dag_list();
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
