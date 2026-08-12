// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_service_internal.h
 * @brief Internal shared definitions of the scheduler service (task-scheduling
 *        domain + blueprint-scheduling DAG domain).
 * @details Defines struct sched_service (opaque externally; full layout
 *          here) and the DAG task-graph internal structures; declares
 *          cross-file shared helpers. For use only by the sched_service_impl.c
 *          and sched_dag_impl.c translation units.
 */

#ifndef AIRY_RT_SCHED_SERVICE_INTERNAL_H
#define AIRY_RT_SCHED_SERVICE_INTERNAL_H

#include "scheduler_service.h"
#include "platform.h"
#include "thread_pool.h"
#include "multi_agent_collaboration.h"
#include <airymax/sched.h>
#include <cjson/cJSON.h>

/* ============================================================================
 * DAG task-graph internal structures (work-hall mechanism: node deps ->
 * topological dispatch -> status board)
 * ============================================================================ */

typedef struct sched_dag_node {
    char *id;
    char *goal;
    char *role;
    char *depends[SCHED_DAG_MAX_DEPS];
    size_t dep_count;
    sched_dag_node_status_t status;
    char *output;
    char *error;
    uint64_t started_at_ms;
    uint64_t finished_at_ms;

    uint32_t max_retries;
    uint32_t retry_delay_ms;
    uint32_t retry_count;
    uint64_t retry_at_ms;

    char *validator_rule_json;
} sched_dag_node_t;

typedef struct sched_dag {
    char *dag_id;
    char *name;
    sched_dag_node_t *nodes[SCHED_DAG_MAX_NODES];
    size_t node_count;
    sched_dag_status_t status;
    size_t terminal_count;
    uint64_t created_at_ms;
    uint64_t finished_at_ms;
    uint64_t retry_budget_ms;
} sched_dag_t;

struct sched_service {
    sched_config_t config;
    agent_info_t *agents[AIRY_CAP_MAX_AGENTS];
    size_t agent_count;
    uint64_t total_tasks_scheduled;
    uint64_t total_success;
    int initialized;

    task_record_t *tasks[AIRY_CAP_MAX_TASKS];
    size_t task_count;
    task_record_t *queue[AIRY_CAP_MAX_TASKS];
    size_t queue_head;
    size_t queue_tail;
    size_t task_seq;
    /* Unified lock: guards the agents array + task queue + task records + DAG
     * table. Worker and RPC threads access them concurrently (the original
     * single-threaded event loop needed no lock; with worker threads the lock
     * is mandatory, otherwise register_agent races with selection reads) */
    airy_mtx_t lock;
    airy_cond_t queue_cond;
    sched_task_executor_t executor;
    volatile int worker_run;
    airy_thread_t worker_thread;

    /* ---- DAG task graph execution engine (work-hall mechanism) ----
     * dags[] are persistent records (like tasks[]: not released during run,
     * cleaned up at destroy); the dag worker thread scans active graphs and
     * dispatches ready nodes one by one to the executor. */
    sched_dag_t *dags[SCHED_DAG_MAX_DAGS];
    size_t dag_count;
    size_t dag_seq;
    airy_cond_t dag_cond;
    volatile int dag_run;
    airy_thread_t dag_thread;

    /* ---- DAG parallel dispatch (mac_framework delegation wiring) ----
     * mac is created at create() time when config.dag_max_parallel>0; agent
     * register/unregister sync to mac; each round the dag worker collects a
     * batch of ready nodes (<= dag_batch_size), mac delegate_batch selects
     * agents -> dag_pool executes concurrently -> complete_task writes back ->
     * node states are written back after the batch barrier.
     * dag_max_parallel==0 keeps the original single-node serial dispatch. */
    mac_framework_t *mac;
    thread_pool_t *dag_pool;
    uint32_t dag_max_parallel;
    uint32_t dag_batch_size;
    bool dag_fatal_cascade;
    volatile size_t batch_pending;
    airy_mtx_t batch_lock;
    airy_cond_t batch_cond;
};

/* Cross-domain shared helpers (used by both sched_service_impl.c and
 * sched_dag_impl.c):
 * sched_now_ms - approximate millisecond clock (shared by the task queue
 *   and the DAG engine, defined in sched_service_impl.c);
 * sched_dag_worker_thread - DAG worker-thread entry (defined in
 *   sched_dag_impl.c, started/stopped by sched_service_start_workers/
 *   stop_workers). */
uint64_t sched_now_ms(void);
void *sched_dag_worker_thread(void *arg);

/* DAG JSON parsing and topological validation (defined in
 * sched_dag_parse.c, called by sched_service_submit_dag): on success
 * returns a newly built sched_dag_t; on failure rolls back internally and
 * returns the specific error code. */
int sched_dag_validate_and_build(cJSON *root, sched_dag_t **out_dag);

#endif /* AIRY_RT_SCHED_SERVICE_INTERNAL_H */
