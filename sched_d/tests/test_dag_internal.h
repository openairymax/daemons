// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_dag_internal.h
 * @brief test_dag 拆分后各测试文件共享的声明与宏
 */

#ifndef TEST_DAG_INTERNAL_H
#define TEST_DAG_INTERNAL_H

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

/* ---- 共享全局测试状态（定义于 test_dag.c，各域文件经此访问） ---- */
extern char g_exec_log[64][256];
extern size_t g_exec_count;
extern const char *g_fail_goal;
extern const char *g_fatal_goal;
extern const char *g_flaky_goal;
extern int g_flaky_left;
extern volatile int g_block;
extern int g_concurrent_now;
extern int g_concurrent_max;

/* ---- 共享辅助函数（定义于 test_dag.c） ---- */
void exec_log_push(const char *role, const char *goal);
int fake_executor(const char *agent_id, const char *task_description, const char *workspace_dir,
                  char **out_output);
sched_service_t *make_service(void);
sched_service_t *make_service_graded(bool fatal_cascade, uint32_t parallel);
sched_service_t *make_parallel_service(void);
int wait_dag_terminal(sched_service_t *svc, const char *dag_id, int timeout_ms);
int get_dag_status_str(sched_service_t *svc, const char *dag_id, char *out, size_t cap);
int parallel_executor(const char *agent_id, const char *task_description,
                      const char *workspace_dir, char **out_output);

/* ---- 各域文件的测试函数（main() 直接调用，调用语句保持原样） ---- */
/* test_dag_core.c */
int test_dag_topological_order(void);
int test_dag_cycle_detection(void);
int test_dag_invalid_node_ids(void);
int test_priority_queue_order(void);
int test_dag_input_fallback(void);
int test_dag_list(void);
/* test_dag_failure.c */
int test_dag_failure_cascade(void);
int test_dag_cancel(void);
int test_dag_normal_failure_no_cascade(void);
int test_dag_fatal_cascade_whole(void);
int test_dag_transient_retry(void);
int test_dag_transient_retry_exhausted(void);
/* test_dag_parallel.c */
int test_dag_parallel_delegation(void);
int test_dag_group_consensus_collab(void);

#endif /* TEST_DAG_INTERNAL_H */
