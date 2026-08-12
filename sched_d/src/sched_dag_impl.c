// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_dag_impl.c
 * @brief 调度服务 · 蓝图调度（DAG）域实现
 * @details 工作大厅机制：实现 scheduler_service.h 中 DAG 域公共 API
 *          （submit_dag/get_dag/cancel_dag/checkpoint_save）与 sched_dag_*
 *          内部引擎（依赖解析、并行批派发、失败分级、重试背压、拓扑校验）。
 */

#include "sched_service_internal.h"
#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"
#include "airy_artifact_validator.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <cjson/cJSON.h>

/* ============================================================================
 * DAG task graph execution engine implementation (work-hall mechanism)
 * ============================================================================ */

static int sched_dag_node_ready(const sched_dag_t *dag, size_t idx)
{
    const sched_dag_node_t *node = dag->nodes[idx];
    if (node->status != SCHED_DAG_NODE_PENDING)
        return 0;
    if (node->retry_at_ms && sched_now_ms() < node->retry_at_ms)
        return 0;
    for (size_t k = 0; k < node->dep_count; k++) {
        const char *dep_id = node->depends[k];
        int dep_ok = 0;
        for (size_t j = 0; j < dag->node_count; j++) {
            if (strcmp(dag->nodes[j]->id, dep_id) == 0) {
                if (dag->nodes[j]->status == SCHED_DAG_NODE_COMPLETED) {
                    dep_ok = 1;
                } else if (dag->nodes[j]->status == SCHED_DAG_NODE_FAILED ||
                           dag->nodes[j]->status == SCHED_DAG_NODE_CANCELED) {

                    return -1;
                }
                break;
            }
        }
        if (!dep_ok) {

            return 0;
        }
    }
    return 1;
}

/* Scan all active graphs: return the dag index of the first ready node
 * (-1 if none). Caller holds the lock. */
static long sched_dag_find_ready(sched_service_t *svc, sched_dag_node_t **out_node)
{
    for (size_t i = 0; i < svc->dag_count; i++) {
        sched_dag_t *dag = svc->dags[i];
        if (dag->status != SCHED_DAG_STATUS_ACTIVE)
            continue;
        for (size_t j = 0; j < dag->node_count; j++) {
            int r = sched_dag_node_ready(dag, j);
            if (r > 0) {
                *out_node = dag->nodes[j];
                return (long)i;
            }
        }
    }
    return -1;
}

static int sched_dag_all_terminal(sched_service_t *svc, sched_dag_t *dag)
{
    for (size_t j = 0; j < dag->node_count; j++) {
        sched_dag_node_status_t st = dag->nodes[j]->status;
        if (st != SCHED_DAG_NODE_COMPLETED && st != SCHED_DAG_NODE_FAILED &&
            st != SCHED_DAG_NODE_CANCELED)
            return 0;
    }
    return 1;
}

/* Error code -> FATAL? (improvement 3: only FATAL cascades cancellation of
 * the whole graph, fail-closed). FATAL class: system-level infrastructure
 * errors such as memory/process/thread/sync primitives — continuing other
 * nodes is pointless and likely to fail repeatedly. Security-layer
 * interceptions (EPERM) are NOT fatal: approval/sandbox denials are passed
 * back up under RESPOND_TO_MODEL semantics to reroute, not to abort the graph. */
static int sched_dag_error_is_fatal(int dret)
{
    switch (dret) {
    case AIRY_ERR_OUT_OF_MEMORY:
    case AIRY_ERR_SYS_RESOURCE:
    case AIRY_ERR_SYS_THREAD:
    case AIRY_ERR_SYS_MUTEX:
    case AIRY_ERR_SYS_SEMAPHORE:
    case AIRY_ERR_SYS_CONDITION:
    case AIRY_ERR_SYS_PIPE:
    case AIRY_ERR_SYS_PROCESS:
    case AIRY_ERR_SYS_SOCKET:
    case AIRY_ERR_SYS_FILE:
        return 1;
    default:
        return 0;
    }
}

/* Error code -> transient? (improvement 4: transient errors can be retried
 * with backoff). transient: environmental transient errors such as
 * timeout/service-unavailable/rate-limit/parse failure — retry is likely to
 * succeed; permanent (syntax error/permission denial/missing dependency) is
 * not retried, avoiding useless retry storms. */
static int sched_dag_error_is_transient(int dret)
{
    switch (dret) {
    case AIRY_ERR_TIMEOUT:
    case AIRY_ERR_EXEC_TIMEOUT:
    case AIRY_ERR_SVC_NOT_READY:
    case AIRY_ERR_SVC_BUSY:
    case AIRY_ERR_SVC_STOPPED:
    case AIRY_ERR_LLM_RATE_LIMIT:
    case AIRY_ERR_LLM_PROVIDER_FAIL:
    case AIRY_ERR_LLM_PARSE_RESP:
    case AIRY_ERR_LLM_EMPTY_RESP:
    case AIRY_ERR_PARSE_ERROR:
    case AIRY_ERR_INTERRUPTED:
    case AIRY_ERR_WOULD_BLOCK:
        return 1;
    default:
        return 0;
    }
}

/* ============================================================================
 * DAG parallel batch dispatch (mac_framework delegation mode)
 * A batch of ready nodes -> delegate_batch selects agents -> thread pool runs
 * the executor concurrently -> completion write-back. Each node is one batch
 * item: the worker runs the executor outside the lock (LLM round-trip), then
 * mac complete_task writes back (releasing the concurrency slot) + node status
 * write-back under the lock (semantics identical to the original serial
 * single-node dispatch: cancel drops / success writes back / failure is
 * graded — FATAL cascades to abort the graph, ordinary failure does not
 * cascade but cancels unreachable downstream, transient retries with backoff).
 * ============================================================================ */

typedef struct sched_dag_batch_item {
    sched_service_t *svc;
    sched_dag_t *dag;
    sched_dag_node_t *node;
    char *agent_id;
} sched_dag_batch_item_t;

/* Node terminal-state write-back (call with lock held, shared by batch
 * workers; returns whether the node succeeded).
 * Ownership contract: this function fully takes over output (transferred to
 * node->output on success; freed on cancel/failure branches), the caller must
 * not free it again.
 *
 * Failure grading (improvements 3/4):
 *   - graph/node already canceled (or graph FAILED/converged) -> drop result
 *   - FATAL (only when dag_fatal_cascade=true) -> node FAILED + cascade-cancel
 *     the whole graph
 *   - transient with retry not exhausted/under budget -> back to PENDING +
 *     exponential backoff
 *   - everything else (ordinary failure / legacy dag_fatal_cascade=false) ->
 *     node FAILED (legacy cascaded the whole graph; new behavior does not
 *     cascade, unreachable downstream is canceled uniformly by
 *     sched_dag_propagate_unreachable) */
static int sched_dag_write_back_node(sched_service_t *svc, sched_dag_t *dag, sched_dag_node_t *node,
                                     int dret, char *output)
{
    if (node->status == SCHED_DAG_NODE_CANCELED || dag->status == SCHED_DAG_STATUS_CANCELED ||
        dag->status == SCHED_DAG_STATUS_FAILED) {

        if (output)
            AIRY_FREE(output);
        if (node->status != SCHED_DAG_NODE_CANCELED)
            node->status = SCHED_DAG_NODE_CANCELED;
        node->finished_at_ms = sched_now_ms();
        SVC_LOG_INFO("sched: DAG node %s/%s result discarded (dag canceled/failed)", dag->dag_id,
                     node->id);
        return 0;
    }
    if (dret == AIRY_SUCCESS && output) {
        /* Improvement 2 (P2a): artifact verification (before write_back).
         * When a node declares validator rules, run deterministic validation
         * (built via air_artifact_validator_from_json):
         *   - PASS     -> normal write-back (current behavior)
         *   - FAIL     -> node FAILED + "verification failed" tag (deterministic
         *                 failure, permanent, not retried; no graph cascade,
         *                 same semantics as ordinary failure)
         *   - SKIP     -> no rules, write back directly
         * Validation input limits: write_back only holds dret+output; exit_code
         * is implied by the SUCCESS branch, artifact_exists/diff_scope rules are
         * skipped due to missing artifact paths/change sets. */
        if (node->validator_rule_json && node->validator_rule_json[0]) {
            airy_artifact_validator_t *av = NULL;
            if (airy_artifact_validator_from_json(&av, node->validator_rule_json) == AIRY_SUCCESS && av) {
                airy_artifact_meta_t vmeta;
                __builtin_memset(&vmeta, 0, sizeof(vmeta));
                vmeta.exit_code = 0;
                vmeta.output = output;
                airy_artifact_result_t vres;
                __builtin_memset(&vres, 0, sizeof(vres));
                airy_artifact_validator_validate(av, &vmeta, &vres);
                airy_artifact_validator_destroy(av);
                if (vres.verify == AIRY_AV_FAIL) {
                    node->status = SCHED_DAG_NODE_FAILED;
                    if (node->error) {
                        AIRY_FREE(node->error);
                        node->error = NULL;
                    }
                    node->error = AIRY_STRDUP("artifact verification failed");
                    node->retry_at_ms = 0;
                    node->finished_at_ms = sched_now_ms();
                    SVC_LOG_ERROR("DAG node verification FAILED: %s/%s (%s)", dag->dag_id, node->id,
                                  vres.reason);
                    AIRY_FREE(output);
                    return 0;
                }
            }
        }
        node->status = SCHED_DAG_NODE_COMPLETED;
        node->output = output;
        if (node->error) {
            AIRY_FREE(node->error);
            node->error = NULL;
        }
        node->retry_at_ms = 0;
        node->finished_at_ms = sched_now_ms();
        SVC_LOG_INFO("DAG node completed: %s/%s (output_len=%zu)", dag->dag_id, node->id,
                     node->output ? strlen(node->output) : 0);
        return 1;
    }
    node->finished_at_ms = sched_now_ms();

    if (node->error) {
        AIRY_FREE(node->error);
        node->error = NULL;
    }

    if (svc->dag_fatal_cascade && sched_dag_error_is_fatal(dret)) {
        node->status = SCHED_DAG_NODE_FAILED;
        node->error = AIRY_STRDUP("agent dispatch failed (fatal)");
        SVC_LOG_ERROR("DAG node FATAL failed: %s/%s (error=%s, rc=%d)", dag->dag_id, node->id,
                      node->error ? node->error : "?", dret);
        dag->status = SCHED_DAG_STATUS_FAILED;
        size_t cascade = 0;
        for (size_t j = 0; j < dag->node_count; j++) {
            sched_dag_node_t *n = dag->nodes[j];
            if (n->status == SCHED_DAG_NODE_PENDING || n->status == SCHED_DAG_NODE_READY) {
                n->status = SCHED_DAG_NODE_CANCELED;
                n->finished_at_ms = sched_now_ms();
                cascade++;
            }
        }
        SVC_LOG_WARN("DAG aborted by FATAL node failure: %s (%zu nodes cascade-canceled)",
                     dag->dag_id, cascade);
        if (output)
            AIRY_FREE(output);
        return 0;
    }

    if (!svc->dag_fatal_cascade) {
        node->status = SCHED_DAG_NODE_FAILED;
        node->error = AIRY_STRDUP(dret == AIRY_ERR_SVC_NOT_READY ? "dag executor not injected" :
                                                                   "agent dispatch failed");
        SVC_LOG_ERROR("DAG node failed: %s/%s (error=%s)", dag->dag_id, node->id,
                      node->error ? node->error : "unknown");
        dag->status = SCHED_DAG_STATUS_FAILED;
        size_t cascade = 0;
        for (size_t j = 0; j < dag->node_count; j++) {
            sched_dag_node_t *n = dag->nodes[j];
            if (n->status == SCHED_DAG_NODE_PENDING || n->status == SCHED_DAG_NODE_READY) {
                n->status = SCHED_DAG_NODE_CANCELED;
                n->finished_at_ms = sched_now_ms();
                cascade++;
            }
        }
        SVC_LOG_WARN("DAG aborted by node failure: %s (%zu downstream nodes "
                     "cascade-canceled)",
                     dag->dag_id, cascade);
        if (output)
            AIRY_FREE(output);
        return 0;
    }

    if (sched_dag_error_is_transient(dret) && node->retry_count < node->max_retries) {
        uint64_t now = sched_now_ms();
        uint64_t budget_deadline =
            dag->retry_budget_ms ? dag->created_at_ms + dag->retry_budget_ms : 0;
        if (budget_deadline == 0 || now < budget_deadline) {
            node->retry_count++;
            uint64_t base = node->retry_delay_ms ? node->retry_delay_ms : 1000;
            uint64_t backoff = base * (1ULL << (node->retry_count - 1));
            node->retry_at_ms = now + backoff;
            node->error = AIRY_STRDUP("transient failure, retrying with backoff");
            node->status = SCHED_DAG_NODE_PENDING;
            node->finished_at_ms = 0;
            SVC_LOG_WARN("DAG node transient failure, retry %u/%u backoff=%llu ms: %s/%s",
                         node->retry_count, node->max_retries, (unsigned long long)backoff,
                         dag->dag_id, node->id);
            if (output)
                AIRY_FREE(output);
            return 0;
        }
    }

    node->status = SCHED_DAG_NODE_FAILED;
    node->error = AIRY_STRDUP("agent dispatch failed");
    SVC_LOG_ERROR("DAG node failed (no cascade): %s/%s (error=%s, rc=%d)", dag->dag_id, node->id,
                  node->error ? node->error : "unknown", dret);
    if (output)
        AIRY_FREE(output);
    return 0;
}

/* Dependency-failure propagation: a PENDING node with any dependency
 * FAILED/CANCELED becomes CANCELED. When ordinary failures do not cascade the
 * whole graph, nodes with failed dependencies never become ready; without this
 * handling the dag thread would wait forever. This function iteratively marks
 * all unreachable nodes so the graph converges. Call with lock held. */
static void sched_dag_propagate_unreachable(sched_service_t *svc)
{
    int changed;
    do {
        changed = 0;
        for (size_t i = 0; i < svc->dag_count; i++) {
            sched_dag_t *dag = svc->dags[i];
            if (dag->status != SCHED_DAG_STATUS_ACTIVE)
                continue;
            for (size_t j = 0; j < dag->node_count; j++) {
                sched_dag_node_t *node = dag->nodes[j];
                if (node->status != SCHED_DAG_NODE_PENDING)
                    continue;
                for (size_t k = 0; k < node->dep_count; k++) {
                    for (size_t m = 0; m < dag->node_count; m++) {
                        if (strcmp(dag->nodes[m]->id, node->depends[k]) != 0)
                            continue;
                        sched_dag_node_status_t dst = dag->nodes[m]->status;
                        if (dst == SCHED_DAG_NODE_FAILED || dst == SCHED_DAG_NODE_CANCELED) {
                            node->status = SCHED_DAG_NODE_CANCELED;
                            node->finished_at_ms = sched_now_ms();
                            changed = 1;
                        }
                        break;
                    }
                }
            }
        }
    } while (changed);
}

/* Graph terminal-state convergence: finalized once all nodes of every active
 * graph reach a terminal state. Any FAILED node -> graph FAILED (reporting
 * partial failure faithfully); all COMPLETED -> COMPLETED. Call with lock
 * held. */
static void sched_dag_finalize_terminal(sched_service_t *svc)
{
    for (size_t i = 0; i < svc->dag_count; i++) {
        sched_dag_t *dag = svc->dags[i];
        if (dag->status != SCHED_DAG_STATUS_ACTIVE)
            continue;
        if (!sched_dag_all_terminal(svc, dag))
            continue;
        int has_failed = 0;
        for (size_t j = 0; j < dag->node_count; j++) {
            if (dag->nodes[j]->status == SCHED_DAG_NODE_FAILED) {
                has_failed = 1;
                break;
            }
        }
        dag->status = has_failed ? SCHED_DAG_STATUS_FAILED : SCHED_DAG_STATUS_COMPLETED;
        dag->finished_at_ms = sched_now_ms();
        SVC_LOG_INFO("DAG %s: %s (%zu nodes)", dag->dag_id, has_failed ? "failed" : "completed",
                     dag->node_count);
    }
}

/* Smallest retry_at_ms among nodes in backoff wait (PENDING with
 * retry_at_ms > now) across all active graphs; 0 when no node is in backoff.
 * Caller holds the lock. Lets the dag thread wait the shortest backoff
 * instead of busy-polling or blocking forever during backoff periods. */
static uint64_t sched_dag_min_retry_at(sched_service_t *svc)
{
    uint64_t min = 0;
    uint64_t now = sched_now_ms();
    for (size_t i = 0; i < svc->dag_count; i++) {
        sched_dag_t *dag = svc->dags[i];
        if (dag->status != SCHED_DAG_STATUS_ACTIVE)
            continue;
        for (size_t j = 0; j < dag->node_count; j++) {
            sched_dag_node_t *n = dag->nodes[j];
            if (n->status == SCHED_DAG_NODE_PENDING && n->retry_at_ms > now &&
                (min == 0 || n->retry_at_ms < min))
                min = n->retry_at_ms;
        }
    }
    return min;
}

static void sched_dag_batch_worker(void *arg)
{
    sched_dag_batch_item_t *item = (sched_dag_batch_item_t *)arg;
    sched_service_t *svc = item->svc;
    sched_dag_node_t *node = item->node;

    char *output = NULL;
    int dret = svc->executor ? svc->executor(item->agent_id ? item->agent_id : "coding",
                                             node->goal ? node->goal : "", &output) :
                               AIRY_ERR_SVC_NOT_READY;

    if (svc->mac && node->id) {
        mac_framework_complete_task(svc->mac, node->id,
                                    (dret == AIRY_SUCCESS && output) ? output : NULL);
    }

    airy_mtx_lock(&svc->lock);
    sched_dag_write_back_node(svc, item->dag, node, dret, output);

    if (svc->batch_pending > 0) {
        svc->batch_pending--;
        if (svc->batch_pending == 0)
            airy_cond_broadcast(&svc->batch_cond);
    }
    airy_mtx_unlock(&svc->lock);

    AIRY_FREE(item->agent_id);
    AIRY_FREE(item);
}

/* Collect the first batch of ready nodes from all active graphs (<= max) and
 * set them all RUNNING. Caller holds the lock. Returns the count collected;
 * out_dags records the owning graph of each node (aligned with out_nodes). */
static size_t sched_dag_collect_ready_batch(sched_service_t *svc, sched_dag_node_t **out_nodes,
                                            sched_dag_t **out_dags, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < svc->dag_count && n < max; i++) {
        sched_dag_t *dag = svc->dags[i];
        if (dag->status != SCHED_DAG_STATUS_ACTIVE)
            continue;
        for (size_t j = 0; j < dag->node_count && n < max; j++) {
            int r = sched_dag_node_ready(dag, j);
            if (r > 0) {
                sched_dag_node_t *node = dag->nodes[j];
                node->status = SCHED_DAG_NODE_RUNNING;
                node->started_at_ms = sched_now_ms();
                out_nodes[n] = node;
                out_dags[n] = dag;
                n++;
            }
        }
    }
    return n;
}

void *sched_dag_worker_thread(void *arg)
{
    sched_service_t *svc = (sched_service_t *)arg;

    while (1) {
        airy_mtx_lock(&svc->lock);
        while (sched_dag_find_ready(svc, &(sched_dag_node_t *){NULL}) < 0 && svc->dag_run) {
            /* Graded retry (improvement 4): when a node is in backoff wait,
             * wait the shortest backoff; once it expires the node naturally
             * enters the ready set; with no backoff node, block on the signal. */
            uint64_t min_retry = sched_dag_min_retry_at(svc);
            if (min_retry == 0) {
                SVC_LOG_DEBUG("sched: dag worker idle, waiting for ready nodes "
                              "(dags=%zu)",
                              svc->dag_count);
                airy_cond_wait(&svc->dag_cond, &svc->lock);
            } else {
                uint64_t now = sched_now_ms();
                uint32_t wait_ms = (min_retry > now) ? (uint32_t)(min_retry - now) : 1;
                airy_cond_timedwait(&svc->dag_cond, &svc->lock, wait_ms);
            }
        }
        if (!svc->dag_run) {
            SVC_LOG_INFO("sched: dag worker received stop signal, exiting");
            airy_mtx_unlock(&svc->lock);
            break;
        }

        if (svc->mac && svc->dag_pool && svc->dag_max_parallel > 0) {
            sched_dag_node_t *batch[SCHED_DAG_MAX_NODES];
            sched_dag_t *batch_dags[SCHED_DAG_MAX_NODES];
            size_t batch_n =
                sched_dag_collect_ready_batch(svc, batch, batch_dags, svc->dag_batch_size);
            if (batch_n == 0) {
                SVC_LOG_DEBUG("sched: dag worker woke but no ready node (parallel path)");
                airy_mtx_unlock(&svc->lock);
                continue;
            }

            mac_collab_task_t tasks[SCHED_DAG_MAX_NODES];
            char *assigned[SCHED_DAG_MAX_NODES];
            __builtin_memset(tasks, 0, sizeof(tasks));
            __builtin_memset(assigned, 0, sizeof(assigned));
            for (size_t i = 0; i < batch_n; i++) {
                AIRY_STRNCPY_TERM(tasks[i].id, batch[i]->id, sizeof(tasks[i].id));
                tasks[i].id[sizeof(tasks[i].id) - 1] = '\0';
                tasks[i].input_json = batch[i]->goal;
                SVC_LOG_INFO("sched: DAG node dispatch (parallel): %s/%s role=%s deps=%zu",
                             batch_dags[i]->dag_id, batch[i]->id,
                             batch[i]->role ? batch[i]->role : "coding", batch[i]->dep_count);
            }
            int dret = mac_framework_delegate_batch(svc->mac, NULL, tasks, batch_n, assigned);
            if (dret != 0)
                SVC_LOG_WARN("sched: mac delegate_batch partial failure rc=%d", dret);

            svc->batch_pending = batch_n;
            airy_mtx_unlock(&svc->lock);

            /* Submit the batch to the thread pool for concurrent execution
             * (on submit failure, run synchronously as a fallback; ownership
             * of assigned[i] transfers with the item and is freed by the batch
             * worker) */
            for (size_t i = 0; i < batch_n; i++) {
                sched_dag_batch_item_t *item =
                    (sched_dag_batch_item_t *)AIRY_CALLOC(1, sizeof(sched_dag_batch_item_t));
                if (!item) {

                    AIRY_FREE(assigned[i]);
                    assigned[i] = NULL;
                    airy_mtx_lock(&svc->lock);
                    batch[i]->status = SCHED_DAG_NODE_FAILED;
                    batch[i]->error = AIRY_STRDUP("batch item alloc failed");
                    batch[i]->finished_at_ms = sched_now_ms();
                    if (svc->batch_pending > 0) {
                        svc->batch_pending--;
                        if (svc->batch_pending == 0)
                            airy_cond_broadcast(&svc->batch_cond);
                    }
                    airy_mtx_unlock(&svc->lock);
                    continue;
                }
                item->svc = svc;
                item->dag = batch_dags[i];
                item->node = batch[i];
                item->agent_id = assigned[i];
                if (thread_pool_submit(svc->dag_pool, sched_dag_batch_worker, item) != 0) {
                    SVC_LOG_WARN("sched: dag pool submit failed, run synchronously");
                    sched_dag_batch_worker(item);
                }
            }

            airy_mtx_lock(&svc->lock);
            while (svc->batch_pending > 0) {
                airy_cond_wait(&svc->batch_cond, &svc->lock);
            }

            sched_dag_propagate_unreachable(svc);
            sched_dag_finalize_terminal(svc);
            airy_mtx_unlock(&svc->lock);

            airy_mtx_lock(&svc->lock);
            airy_cond_broadcast(&svc->dag_cond);
            airy_mtx_unlock(&svc->lock);
            continue;
        }

        sched_dag_node_t *node = NULL;
        long dag_idx = sched_dag_find_ready(svc, &node);
        if (dag_idx < 0 || !node) {

            SVC_LOG_DEBUG("sched: dag worker woke but no ready node (all dags "
                          "done or deps unresolved)");
            airy_mtx_unlock(&svc->lock);
            continue;
        }
        sched_dag_t *dag = svc->dags[dag_idx];
        node->status = SCHED_DAG_NODE_RUNNING;
        node->started_at_ms = sched_now_ms();

        const char *role = node->role ? node->role : "coding";
        const char *goal = node->goal ? node->goal : "";
        SVC_LOG_INFO("sched: DAG node dispatch: %s/%s role=%s deps=%zu "
                     "(wait since dag create=%llu ms, executor=%s)",
                     dag->dag_id, node->id, role, node->dep_count,
                     (unsigned long long)(sched_now_ms() - dag->created_at_ms),
                     svc->executor ? "ready" : "MISSING");
        airy_mtx_unlock(&svc->lock);

        char *output = NULL;
        int dret = svc->executor ? svc->executor(role, goal, &output) : AIRY_ERR_SVC_NOT_READY;

        airy_mtx_lock(&svc->lock);
        sched_dag_write_back_node(svc, dag, node, dret, output);

        sched_dag_propagate_unreachable(svc);
        sched_dag_finalize_terminal(svc);
        airy_mtx_unlock(&svc->lock);

        airy_mtx_lock(&svc->lock);
        airy_cond_broadcast(&svc->dag_cond);
        airy_mtx_unlock(&svc->lock);
    }
    return NULL;
}

int sched_service_submit_dag(sched_service_t *service, const char *dag_json, char **out_dag_id)
{
    if (!service || !dag_json || !out_dag_id || !service->initialized) {
        SVC_LOG_ERROR("sched_service_submit_dag: NULL parameter or not initialized");
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_dag_id = NULL;

    cJSON *root = cJSON_Parse(dag_json);
    if (!root) {
        SVC_LOG_ERROR("sched_service_submit_dag: invalid DAG JSON");
        return AIRY_ERR_PARSE_ERROR;
    }

    sched_dag_t *dag = NULL;
    int vret = sched_dag_validate_and_build(root, &dag);
    if (vret != AIRY_SUCCESS || !dag) {
        cJSON_Delete(root);
        SVC_LOG_ERROR("sched_service_submit_dag: validation failed (rc=%d)", vret);

        return vret;
    }

    airy_mtx_lock(&service->lock);
    if (service->dag_count >= SCHED_DAG_MAX_DAGS) {
        airy_mtx_unlock(&service->lock);

        for (size_t i = 0; i < dag->node_count; i++) {
            sched_dag_node_t *node = dag->nodes[i];
            AIRY_FREE(node->id);
            AIRY_FREE(node->goal);
            AIRY_FREE(node->role);
            AIRY_FREE(node->validator_rule_json);
            for (size_t k = 0; k < node->dep_count; k++)
                AIRY_FREE(node->depends[k]);
            AIRY_FREE(node);
        }
        AIRY_FREE(dag->name);
        AIRY_FREE(dag);
        cJSON_Delete(root);
        return AIRY_ERR_OVERFLOW;
    }

    char id_buf[64];
    snprintf(id_buf, sizeof(id_buf), "dag_%llu_%zu", (unsigned long long)time(NULL),
             service->dag_seq++);
    dag->dag_id = AIRY_STRDUP(id_buf);
    if (!dag->dag_id) {
        airy_mtx_unlock(&service->lock);
        for (size_t i = 0; i < dag->node_count; i++) {
            sched_dag_node_t *node = dag->nodes[i];
            AIRY_FREE(node->id);
            AIRY_FREE(node->goal);
            AIRY_FREE(node->role);
            AIRY_FREE(node->validator_rule_json);
            for (size_t k = 0; k < node->dep_count; k++)
                AIRY_FREE(node->depends[k]);
            AIRY_FREE(node);
        }
        AIRY_FREE(dag->name);
        AIRY_FREE(dag);
        cJSON_Delete(root);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    dag->status = SCHED_DAG_STATUS_ACTIVE;
    dag->created_at_ms = sched_now_ms();
    service->dags[service->dag_count++] = dag;

    airy_cond_broadcast(&service->dag_cond);
    airy_mtx_unlock(&service->lock);

    *out_dag_id = AIRY_STRDUP(id_buf);
    cJSON_Delete(root);
    SVC_LOG_INFO("DAG submitted: %s name=%s (%zu nodes)", id_buf, dag->name ? dag->name : "?",
                 dag->node_count);

    for (size_t j = 0; j < dag->node_count; j++) {
        const sched_dag_node_t *node = dag->nodes[j];
        char depbuf[256];
        size_t off = 0;
        depbuf[0] = '\0';
        for (size_t k = 0; k < node->dep_count && off < sizeof(depbuf) - 2; k++) {
            int w = snprintf(depbuf + off, sizeof(depbuf) - off, "%s%s", k > 0 ? "," : "",
                             node->depends[k]);
            if (w < 0)
                break;
            off += (size_t)w;
        }
        SVC_LOG_DEBUG("DAG %s node[%zu]: id=%s role=%s depends=[%s] goal_len=%zu", id_buf, j,
                      node->id, node->role ? node->role : "?", depbuf,
                      node->goal ? strlen(node->goal) : 0);
    }
    return AIRY_SUCCESS;
}

int sched_service_get_dag(sched_service_t *service, const char *dag_id, char **out_json)
{
    if (!service || !dag_id || !out_json || !service->initialized) {
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_json = NULL;

    airy_mtx_lock(&service->lock);
    sched_dag_t *dag = NULL;
    for (size_t i = 0; i < service->dag_count; i++) {
        if (strcmp(service->dags[i]->dag_id, dag_id) == 0) {
            dag = service->dags[i];
            break;
        }
    }
    if (!dag) {
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_NOT_FOUND;
    }

    static const char *dag_status_names[] = {"active", "completed", "failed", "canceled"};
    static const char *node_status_names[] = {"pending",   "ready",  "running",
                                              "completed", "failed", "canceled"};

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "dag_id", dag->dag_id);
        cJSON_AddStringToObject(root, "name", dag->name ? dag->name : "");
        cJSON_AddStringToObject(root, "status",
                                dag_status_names[dag->status % SCHED_DAG_STATUS_COUNT]);
        cJSON_AddNumberToObject(root, "node_count", (double)dag->node_count);
        size_t done = 0;
        for (size_t j = 0; j < dag->node_count; j++) {
            sched_dag_node_status_t st = dag->nodes[j]->status;
            if (st == SCHED_DAG_NODE_COMPLETED || st == SCHED_DAG_NODE_FAILED ||
                st == SCHED_DAG_NODE_CANCELED)
                done++;
        }
        cJSON_AddNumberToObject(root, "progress", (double)done);
        cJSON_AddNumberToObject(root, "created_at_ms", (double)dag->created_at_ms);
        cJSON_AddNumberToObject(root, "finished_at_ms", (double)dag->finished_at_ms);
        cJSON_AddNumberToObject(root, "retry_budget_ms", (double)dag->retry_budget_ms);

        cJSON *nodes = cJSON_CreateArray();
        for (size_t j = 0; j < dag->node_count; j++) {
            sched_dag_node_t *node = dag->nodes[j];
            cJSON *nj = cJSON_CreateObject();
            cJSON_AddStringToObject(nj, "id", node->id);
            cJSON_AddStringToObject(nj, "goal", node->goal ? node->goal : "");
            cJSON_AddStringToObject(nj, "role", node->role ? node->role : "");
            cJSON_AddStringToObject(nj, "status",
                                    node_status_names[node->status % SCHED_DAG_NODE_COUNT]);
            cJSON *deps = cJSON_CreateArray();
            for (size_t k = 0; k < node->dep_count; k++)
                cJSON_AddItemToArray(deps, cJSON_CreateString(node->depends[k]));
            cJSON_AddItemToObject(nj, "depends", deps);
            if (node->output)
                cJSON_AddStringToObject(nj, "output", node->output);
            if (node->error)
                cJSON_AddStringToObject(nj, "error", node->error);
            cJSON_AddNumberToObject(nj, "started_at_ms", (double)node->started_at_ms);
            cJSON_AddNumberToObject(nj, "finished_at_ms", (double)node->finished_at_ms);
            if (node->max_retries > 0) {
                cJSON_AddNumberToObject(nj, "max_retries", (double)node->max_retries);
                cJSON_AddNumberToObject(nj, "retry_count", (double)node->retry_count);
            }
            cJSON_AddItemToArray(nodes, nj);
        }
        cJSON_AddItemToObject(root, "nodes", nodes);
    }
    airy_mtx_unlock(&service->lock);

    if (!root)
        return AIRY_ERR_OUT_OF_MEMORY;
    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? AIRY_SUCCESS : AIRY_ERR_OUT_OF_MEMORY;
}

int sched_service_cancel_dag(sched_service_t *service, const char *dag_id)
{
    if (!service || !dag_id || !service->initialized) {
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    sched_dag_t *dag = NULL;
    for (size_t i = 0; i < service->dag_count; i++) {
        if (strcmp(service->dags[i]->dag_id, dag_id) == 0) {
            dag = service->dags[i];
            break;
        }
    }
    if (!dag) {
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_NOT_FOUND;
    }
    if (dag->status != SCHED_DAG_STATUS_ACTIVE) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_WARN("sched_service_cancel_dag: dag %s not active (status=%d)", dag_id,
                     (int)dag->status);
        return AIRY_ERR_BUSY;
    }

    dag->status = SCHED_DAG_STATUS_CANCELED;
    dag->finished_at_ms = sched_now_ms();
    size_t canceled_nodes = 0, running_nodes = 0;
    for (size_t j = 0; j < dag->node_count; j++) {
        sched_dag_node_t *node = dag->nodes[j];
        if (node->status == SCHED_DAG_NODE_PENDING || node->status == SCHED_DAG_NODE_READY) {
            node->status = SCHED_DAG_NODE_CANCELED;
            node->finished_at_ms = sched_now_ms();
            node->error = AIRY_STRDUP("canceled by user");
            canceled_nodes++;
        } else if (node->status == SCHED_DAG_NODE_RUNNING) {
            running_nodes++;
        }
    }
    airy_cond_broadcast(&service->dag_cond);
    airy_mtx_unlock(&service->lock);

    SVC_LOG_INFO("DAG canceled: %s (%zu nodes canceled, %zu still running — "
                 "outputs will be discarded on completion)",
                 dag_id, canceled_nodes, running_nodes);
    return AIRY_SUCCESS;
}

int sched_service_checkpoint_save(sched_service_t *service, char **out_json)
{
    if (!service || !out_json || !service->initialized) {
        SVC_LOG_ERROR("sched_service_checkpoint_save: NULL parameter or not initialized");
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_json = NULL;

    airy_mtx_lock(&service->lock);
    size_t pending = 0, running = 0;
    size_t qi = service->queue_head;
    while (qi != service->queue_tail) {
        task_status_t st = service->queue[qi]->status;
        if (st == SCHED_TASK_STATUS_PENDING)
            pending++;
        else if (st == SCHED_TASK_STATUS_RUNNING)
            running++;
        qi = (qi + 1) % AIRY_CAP_MAX_TASKS;
    }
    size_t active_dags = 0, completed_dags = 0;
    size_t dag_total = service->dag_count;
    for (size_t i = 0; i < dag_total; i++) {
        sched_dag_status_t ds = service->dags[i]->status;
        if (ds == SCHED_DAG_STATUS_ACTIVE)
            active_dags++;
        else if (ds == SCHED_DAG_STATUS_COMPLETED)
            completed_dags++;
    }

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddNumberToObject(root, "agent_count", (double)service->agent_count);
        cJSON_AddNumberToObject(root, "total_tasks", (double)service->total_tasks_scheduled);
        cJSON_AddNumberToObject(root, "pending", (double)pending);
        cJSON_AddNumberToObject(root, "running", (double)running);
        cJSON_AddNumberToObject(root, "dag_count", (double)service->dag_count);
        cJSON_AddNumberToObject(root, "active_dags", (double)active_dags);
        cJSON_AddNumberToObject(root, "completed_dags", (double)completed_dags);
        cJSON_AddNumberToObject(root, "timestamp_ms", (double)sched_now_ms());
    }
    airy_mtx_unlock(&service->lock);

    if (!root)
        return AIRY_ERR_OUT_OF_MEMORY;
    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    SVC_LOG_INFO("Checkpoint saved (pending=%zu, running=%zu, dags=%zu)", pending, running,
                 dag_total);
    return *out_json ? AIRY_SUCCESS : AIRY_ERR_OUT_OF_MEMORY;
}
