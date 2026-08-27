// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_dag_engine.c
 * @brief Scheduler service - DAG execution engine domain.
 * @details Core dependency-resolution / failure-grading / retry-backoff /
 *          graph-convergence engine shared by the serial dag worker and the
 *          parallel batch workers (sched_dag_worker.c). The helpers declared
 *          in sched_dag_internal.h were promoted from static when
 *          sched_dag_impl.c was split by functional domain; all of them
 *          expect the caller to hold service->lock. The DAG public APIs
 *          (submit/get/cancel/checkpoint) live in sched_dag_impl.c, JSON
 *          parsing/validation in sched_dag_parse.c.
 */

#include "sched_service_internal.h"
#include "sched_dag_internal.h"
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

/* Agent input for a node: the node goal when it carries real intent, else the
 * graph-level task input. A goal that equals the node id (e.g. "reactive_1_step1")
 * or is empty is only a plan label, not a task description. */
const char *sched_dag_agent_input(const sched_dag_t *dag, const sched_dag_node_t *node)
{
    if (node->goal && node->goal[0] && strcmp(node->goal, node->id) != 0)
        return node->goal;
    return (dag->input && dag->input[0]) ? dag->input : "";
}

int sched_dag_node_ready(const sched_dag_t *dag, size_t idx)
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
long sched_dag_find_ready(sched_service_t *svc, sched_dag_node_t **out_node)
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
 * DAG node terminal-state write-back / failure grading / graph convergence
 * ============================================================================ */

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
int sched_dag_write_back_node(sched_service_t *svc, sched_dag_t *dag, sched_dag_node_t *node,
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
void sched_dag_propagate_unreachable(sched_service_t *svc)
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
void sched_dag_finalize_terminal(sched_service_t *svc)
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
uint64_t sched_dag_min_retry_at(sched_service_t *svc)
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
