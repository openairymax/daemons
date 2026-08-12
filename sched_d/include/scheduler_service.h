/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file scheduler_service.h
 * @brief Scheduler service interface definitions.
 * @details Responsible for task scheduling and selecting the most suitable agent.
 */

#ifndef AIRY_RT_SCHEDULER_SERVICE_H
#define AIRY_RT_SCHEDULER_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Scheduling-strategy type. */
typedef enum {
    SCHED_STRATEGY_ROUND_ROBIN,
    SCHED_STRATEGY_WEIGHTED,
    SCHED_STRATEGY_ML_BASED,
    SCHED_STRATEGY_PRIORITY_BASED,
    SCHED_STRATEGY_COUNT
} sched_strategy_t;

/** @brief Task priority. */
typedef enum {
    TASK_PRIORITY_LOW,
    TASK_PRIORITY_NORMAL,
    TASK_PRIORITY_HIGH,
    TASK_PRIORITY_URGENT,
    TASK_PRIORITY_COUNT
} task_priority_t;

/** @brief Task info. */
typedef struct {
    char *task_id;
    char *task_description;
    task_priority_t priority;
    uint32_t timeout_ms;
    void *task_data;
    size_t task_data_size;
} task_info_t;

/**
 * @brief Task lifecycle status (async queue: enqueue -> selected -> execute
 *        -> done/failed).
 * @note Named with the SCHED_ prefix: types.h already occupies the
 *       TASK_STATUS_* macros, avoiding enum-name expansion conflicts.
 */
typedef enum {
    SCHED_TASK_STATUS_PENDING,
    SCHED_TASK_STATUS_RUNNING,
    SCHED_TASK_STATUS_COMPLETED,
    SCHED_TASK_STATUS_FAILED,
    SCHED_TASK_STATUS_CANCELED,
    SCHED_TASK_STATUS_COUNT
} task_status_t;

/** @brief Task record (queue entry; basis for get_task queries). */
typedef struct {
    char *task_id;
    char *task_description;
    task_priority_t priority;
    uint32_t timeout_ms;
    task_status_t status;
    char *selected_agent_id;
    char *output;
    char *error;
    uint64_t created_at_ms;
    uint64_t finished_at_ms;
} task_record_t;

/**
 * @brief Task-execution callback (injected by the daemon: select agent +
 *        spawn + invoke).
 * @param agent_id Selected agent (role)
 * @param task_description Task description (used as invoke's input)
 * @param out_output Execution output (AIRY_MALLOC, caller frees)
 * @return 0 on success, non-zero on failure
 */
typedef int (*sched_task_executor_t)(const char *agent_id, const char *task_description,
                                     char **out_output);


#define AIRY_CAP_MAX_TASKS 256

/** @brief Agent info. */
typedef struct {
    char *agent_id; /**< Agent ID */
    char *agent_name;
    float load_factor;
    float success_rate;
    uint32_t avg_response_time_ms;
    bool is_available;
    float weight;
} agent_info_t;

/** @brief Scheduling result. */
typedef struct {
    char *selected_agent_id;
    float confidence;
    uint32_t estimated_time_ms;
} sched_result_t;

/** @brief Scheduler service config. */
typedef struct {
    sched_strategy_t strategy;
    uint32_t health_check_interval_ms;
    uint32_t stats_report_interval_ms;
    bool enable_ml_strategy;
    char *ml_model_path;
    uint32_t max_agents;
    /* ---- DAG parallel dispatch (mac_framework delegation wiring;
     * 0 = keep serial) ----
     * dag_max_parallel: max ready nodes dispatched at once per round
     *                   (<= SCHED_DAG_MAX_NODES). 0 = keep the existing
     *                   single-node serial dispatch (legacy behavior).
     * dag_batch_size:   ready-node batch size per round (<= dag_max_parallel,
     *                   0 = default to dag_max_parallel). Batches are
     *                   delegated via mac_framework -> thread-pool
     *                   concurrent execution -> results gathered back into
     *                   node states. */
    uint32_t dag_max_parallel;
    uint32_t dag_batch_size;
    /* ---- Failure-tier semantics (improvement 3: Codex Fatal/normal
     * three-state converged at the sched layer) ----
     * dag_fatal_cascade: true (production default) -> only FATAL-class
     * failures cascade-cancel the whole graph (fail-closed); ordinary
     * failures only mark the node FAILED and cancel its now-unreachable
     * downstream, while other independent branches continue. false -> any
     * node failure cascades-cancels the whole graph (legacy behavior). */
    bool dag_fatal_cascade;
} sched_config_t;

/** @brief Scheduler service handle. */
typedef struct sched_service sched_service_t;

/**
 * @brief Create a scheduler service.
 * @param manager Config info
 * @param service Output parameter, returns the created service handle
 * @return 0 on success, non-zero error code
 */
int sched_service_create(const sched_config_t *manager, sched_service_t **service);

/**
 * @brief Destroy a scheduler service.
 * @param service Service handle
 * @return 0 on success, non-zero error code
 */
int sched_service_destroy(sched_service_t *service);

/**
 * @brief Register an agent.
 * @param service Service handle
 * @param agent_info Agent info
 * @return 0 on success, non-zero error code
 */
int sched_service_register_agent(sched_service_t *service, const agent_info_t *agent_info);

/**
 * @brief Unregister an agent.
 * @param service Service handle
 * @param agent_id Agent ID
 * @return 0 on success, non-zero error code
 */
int sched_service_unregister_agent(sched_service_t *service, const char *agent_id);

/**
 * @brief Update an agent's status.
 * @param service Service handle
 * @param agent_info Agent info
 * @return 0 on success, non-zero error code
 */
int sched_service_update_agent_status(sched_service_t *service, const agent_info_t *agent_info);

/**
 * @brief Schedule a task.
 * @param service Service handle
 * @param task_info Task info
 * @param result Output parameter, returns the scheduling result
 * @return 0 on success, non-zero error code
 */
int sched_service_schedule_task(sched_service_t *service, const task_info_t *task_info,
                                sched_result_t **result);

/**
 * @brief Get scheduler statistics.
 * @param service Service handle
 * @param stats Output parameter, returns statistics
 * @return 0 on success, non-zero error code
 */
int sched_service_get_stats(sched_service_t *service, void **stats);

/**
 * @brief Health check.
 * @param service Service handle
 * @param health_status Output parameter, returns health status
 * @return 0 on success, non-zero error code
 */
int sched_service_health_check(sched_service_t *service, bool *health_status);

/**
 * @brief Reload the config.
 * @param service Service handle
 * @param manager New config info
 * @return 0 on success, non-zero error code
 */
int sched_service_reload_config(sched_service_t *service, const sched_config_t *manager);

/**
 * @brief Submit a task (async queue): returns immediately after enqueue;
 *        the worker thread executes it later.
 * @param service Service handle
 * @param task_info Task info (NULL task_id is generated server-side)
 * @param out_task_id Output parameter, returns the effective task ID (AIRY_MALLOC, caller AIRY_FREEs)
 * @return 0 on success, non-zero error code
 */
int sched_service_submit_task(sched_service_t *service, const task_info_t *task_info,
                              char **out_task_id);

/**
 * @brief Query task status.
 * @param service Service handle
 * @param task_id Task ID
 * @param out_json Output parameter, returns task-status JSON (AIRY_MALLOC, caller AIRY_FREEs)
 * @return 0 on success; AIRY_ERR_NOT_FOUND if the task does not exist
 */
int sched_service_get_task(sched_service_t *service, const char *task_id, char **out_json);

/**
 * @brief Inject the task-execution callback (must be called before start_workers).
 * @param service Service handle
 * @param executor Execution callback (called by a worker thread after selecting an agent)
 * @return 0 on success, non-zero error code
 */
int sched_service_set_executor(sched_service_t *service, sched_task_executor_t executor);

/**
 * @brief Start the task-queue worker threads (consume the pending queue and execute).
 * @param service Service handle
 * @return 0 on success, non-zero error code
 */
int sched_service_start_workers(sched_service_t *service);

/**
 * @brief Stop the task-queue worker threads (call before destroy; idempotent).
 * @param service Service handle
 */
void sched_service_stop_workers(sched_service_t *service);

/**
 * @brief Cancel a task (only PENDING is cancelable; RUNNING/terminal states return AIRY_ERR_BUSY).
 * @param service Service handle
 * @param task_id Task ID
 * @return 0 on success; AIRY_ERR_NOT_FOUND task missing; AIRY_ERR_BUSY not cancelable
 */
int sched_service_cancel_task(sched_service_t *service, const char *task_id);

/* ============================================================================
 * DAG task-graph execution engine (work-hall mechanism, see 08-work-hall.md)
 *
 * Submit a task graph with dependencies: the dag worker thread dispatches
 * ready nodes (all deps completed) in topological order; each node really
 * executes via the injected executor (sched_dispatch_executor -> agent_d
 * spawn/invoke). A node failure aborts the graph and cancels its remaining
 * unfinished nodes. Data source: think_d's GCCP+GRAD plan
 * (nodes: id/goal/depends[role]).
 * ============================================================================ */


typedef enum {
    SCHED_DAG_NODE_PENDING = 0,
    SCHED_DAG_NODE_READY,
    SCHED_DAG_NODE_RUNNING,
    SCHED_DAG_NODE_COMPLETED,
    SCHED_DAG_NODE_FAILED,
    SCHED_DAG_NODE_CANCELED,
    SCHED_DAG_NODE_COUNT
} sched_dag_node_status_t;


typedef enum {
    SCHED_DAG_STATUS_ACTIVE = 0,
    SCHED_DAG_STATUS_COMPLETED,
    SCHED_DAG_STATUS_FAILED,
    SCHED_DAG_STATUS_CANCELED,
    SCHED_DAG_STATUS_COUNT
} sched_dag_status_t;


#define SCHED_DAG_MAX_NODES 64
#define SCHED_DAG_MAX_DEPS 8
#define SCHED_DAG_MAX_DAGS 32

/**
 * @brief Submit a DAG task graph (async: the dag worker thread executes it
 *        topologically after insertion).
 * @param service Service handle
 * @param dag_json Graph-description JSON string:
 *   {"name":"...","nodes":[{"id":"S_01","goal":"...","role":"coding",
 *    "depends":["S_02",...]}, ...]}
 *   - role defaults to "coding"; depends defaults to empty (entry node)
 *   - the graph must be acyclic (Kahn topological check; cycles return AIRY_ERR_CYCLE_DETECTED)
 * @param out_dag_id Output parameter, returns the dag_id (AIRY_MALLOC, caller AIRY_FREEs)
 * @return 0 on success; AIRY_ERR_INVALID_PARAM invalid JSON/empty nodes/over caps;
 *         AIRY_ERR_CYCLE_DETECTED dependency cycle exists
 */
int sched_service_submit_dag(sched_service_t *service, const char *dag_json, char **out_dag_id);

/**
 * @brief Query DAG status (board snapshot: graph status/node status/progress/output).
 * @param service Service handle
 * @param dag_id DAG ID
 * @param out_json Output parameter, returns JSON (AIRY_MALLOC, caller AIRY_FREEs)
 * @return 0 on success; AIRY_ERR_NOT_FOUND does not exist
 */
int sched_service_get_dag(sched_service_t *service, const char *dag_id, char **out_json);

/**
 * @brief Cancel a DAG (all unfinished nodes set to canceled; a RUNNING node
 *        no longer posts output after finishing).
 * @param service Service handle
 * @param dag_id DAG ID
 * @return 0 on success; AIRY_ERR_NOT_FOUND does not exist
 */
int sched_service_cancel_dag(sched_service_t *service, const char *dag_id);

/**
 * @brief Save a scheduler checkpoint (L2 protocol standard method
 *        sched.checkpoint_save).
 * @param service Service handle
 * @param out_json Output parameter, returns the current queue/DAG state
 *        snapshot JSON (AIRY_MALLOC, caller AIRY_FREEs):
 *   {"agent_count":N,"total_tasks":T,"pending":P,"running":R,
 *    "dag_count":D,"active_dags":A,"completed_dags":C,"timestamp_ms":...}
 * @return 0 on success; AIRY_ERR_INVALID_PARAM bad args
 * @note The snapshot is an in-memory view (not persisted); the upper layer
 *       (e.g. monit_d / cluster manager) decides persistence; used for
 *       scheduling-state observation and pre-recovery state export.
 */
int sched_service_checkpoint_save(sched_service_t *service, char **out_json);

#endif /* AIRY_RT_SCHEDULER_SERVICE_H */
