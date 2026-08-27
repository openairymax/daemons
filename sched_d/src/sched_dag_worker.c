// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_dag_worker.c
 * @brief Scheduler service - DAG worker thread / parallel batch dispatch
 *        domain.
 * @details Implements sched_dag_worker_thread (serial node dispatch + the
 *          mac_framework delegation-mode parallel batch path) and its batch
 *          machinery (batch item, batch worker, ready-batch collector).
 *          The engine helpers it calls (node readiness, write-back, failure
 *          propagation, graph convergence, backoff wait) live in
 *          sched_dag_engine.c and are declared in sched_dag_internal.h.
 *          Moved out of sched_dag_impl.c (single-responsibility split); the
 *          DAG public APIs live in sched_dag_impl.c.
 */

#include "sched_service_internal.h"
#include "sched_dag_internal.h"
#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"
#include "thread_pool.h"
#include "multi_agent_collaboration.h"

#include <string.h>
#include <time.h>

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

static void sched_dag_batch_worker(void *arg)
{
    sched_dag_batch_item_t *item = (sched_dag_batch_item_t *)arg;
    sched_service_t *svc = item->svc;
    sched_dag_node_t *node = item->node;

    char *output = NULL;
    int dret = svc->executor ? svc->executor(item->agent_id ? item->agent_id : "coding",
                                             sched_dag_agent_input(item->dag, node),
                                             item->dag->workspace_dir, &output) :
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
                tasks[i].input_json = (char *)sched_dag_agent_input(batch_dags[i], batch[i]);
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
        const char *goal = sched_dag_agent_input(dag, node);
        SVC_LOG_INFO("sched: DAG node dispatch: %s/%s role=%s deps=%zu "
                     "(wait since dag create=%llu ms, executor=%s)",
                     dag->dag_id, node->id, role, node->dep_count,
                     (unsigned long long)(sched_now_ms() - dag->created_at_ms),
                     svc->executor ? "ready" : "MISSING");
        airy_mtx_unlock(&svc->lock);

        char *output = NULL;
        int dret = svc->executor ? svc->executor(role, goal, dag->workspace_dir, &output) :
                                   AIRY_ERR_SVC_NOT_READY;

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
