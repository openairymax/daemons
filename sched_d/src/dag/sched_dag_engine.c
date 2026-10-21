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

/* Node of a graph by id (NULL when absent). Graph layout is immutable after
 * sched_dag_validate_and_build(), so read-only lookups need no ownership. */
static const sched_dag_node_t *sched_dag_find_node(const sched_dag_t *dag, const char *id)
{
    for (size_t j = 0; j < dag->node_count; j++) {
        if (strcmp(dag->nodes[j]->id, id) == 0)
            return dag->nodes[j];
    }
    return NULL;
}

/* Upstream product injection (blueprint semantics B): the direct dependencies
 * that already completed contribute their output excerpt to this node's agent
 * input, so a chained graph really hands intermediate products forward instead
 * of every node working from the bare goal. Release-order note: a node is only
 * ready once its dependencies are COMPLETED, and a COMPLETED node's output is
 * never rewritten, so reading it under service->lock is race-free. */
char *sched_dag_node_input(const sched_dag_t *dag, const sched_dag_node_t *node,
                           size_t budget)
{
    const char *base = sched_dag_agent_input(dag, node);
    size_t base_len = strlen(base);

    const sched_dag_node_t *dep_nodes[SCHED_DAG_MAX_DEPS];
    size_t eligible = 0;
    for (size_t k = 0; k < node->dep_count && k < SCHED_DAG_MAX_DEPS; k++) {
        const sched_dag_node_t *dep = sched_dag_find_node(dag, node->depends[k]);
        if (!dep || dep->status != SCHED_DAG_NODE_COMPLETED || !dep->output || !dep->output[0])
            continue;
        dep_nodes[eligible++] = dep;
    }
    if (eligible == 0)
        return AIRY_STRDUP(base);

    /* Equal share of the budget per dependency: a single oversized product must
     * not crowd the other dependencies out of the graph's shared byte budget. */
    size_t share = budget / eligible;

    size_t dep_take[SCHED_DAG_MAX_DEPS];
    size_t total = base_len + 1;
    size_t used = 0;
    size_t picks = 0;
    for (size_t k = 0; k < eligible; k++) {
        size_t take = strlen(dep_nodes[k]->output);
        if (take > share)
            take = share;
        if (take > budget - used)
            take = budget - used;
        if (take == 0)
            break;
        dep_take[k] = take;
        used += take;
        picks++;
        total += strlen(dep_nodes[k]->id) + take + 32;
    }
    if (picks == 0)
        return AIRY_STRDUP(base);

    char *out = (char *)AIRY_MALLOC(total);
    if (!out)
        return NULL;
    AIRY_MEMCPY(out, base, base_len);
    size_t off = base_len;
    for (size_t k = 0; k < picks; k++) {
        int w = snprintf(out + off, total - off, "\n\n--- 上游产出 [%s] ---\n",
                         dep_nodes[k]->id);
        if (w < 0 || (size_t)w >= total - off)
            break;
        off += (size_t)w;
        AIRY_MEMCPY(out + off, dep_nodes[k]->output, dep_take[k]);
        off += dep_take[k];
    }
    out[off] = '\0';
    return out;
}

int sched_dag_node_ready(const sched_dag_t *dag, size_t idx)
{
    const sched_dag_node_t *node = dag->nodes[idx];
    if (node->status != SCHED_DAG_NODE_PENDING)
        return 0;
    if (node->retry_at_ms && airy_time_wall_ms() < node->retry_at_ms)
        return 0;
    for (size_t k = 0; k < node->dep_count; k++) {
        const sched_dag_node_t *dep = sched_dag_find_node(dag, node->depends[k]);
        if (!dep)
            return 0;
        if (dep->status == SCHED_DAG_NODE_COMPLETED)
            continue;
        return sched_dag_dep_broken(dep->status) ? -1 : 0;
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
        if (!sched_dag_node_done(dag->nodes[j]->status))
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

/* Node error text as one policy: drop the previous value, then rebuild only
 * when a reason is given (NULL clears). */
static void node_err_set(sched_dag_node_t *node, const char *msg)
{
    if (node->error) {
        AIRY_FREE(node->error);
        node->error = NULL;
    }
    if (msg)
        node->error = AIRY_STRDUP(msg);
}

/* Node terminal landing: status, retry clock cleared, finish timestamp. */
static void node_final(sched_dag_node_t *node, sched_dag_node_status_t st)
{
    node->status = st;
    node->retry_at_ms = 0;
    node->finished_at_ms = airy_time_wall_ms();
}

/* Cascade-cancel every still-pending/ready node of a graph that just reached
 * a terminal failure: their dependencies can never be satisfied again, so they
 * are canceled uniformly instead of being left dangling until the graph thread
 * times out. Returns the number of nodes canceled. Call with lock held. */
static size_t dag_cascade_cancel(sched_dag_t *dag)
{
    size_t canceled = 0;
    for (size_t j = 0; j < dag->node_count; j++) {
        sched_dag_node_t *n = dag->nodes[j];
        if (n->status == SCHED_DAG_NODE_PENDING || n->status == SCHED_DAG_NODE_READY) {
            node_final(n, SCHED_DAG_NODE_CANCELED);
            canceled++;
        }
    }
    return canceled;
}

/* Graph already canceled/failed: the result must not be written back, the
 * artifact is released in place. */
static int node_discard(const sched_dag_t *dag, sched_dag_node_t *node, char *out)
{
    AIRY_FREE(out);
    node_final(node, SCHED_DAG_NODE_CANCELED);
    SVC_LOG_INFO("sched: DAG node %s/%s result discarded (dag canceled/failed)", dag->dag_id,
                 node->id);
    return 0;
}

/* Deterministic artifact validation. No declared rules, or a rule set that
 * fails to load, is graded SKIP (pass) exactly as before. 1 = pass, 0 = fail
 * with the validator reason copied into `reason`. */
static int node_verify(const sched_dag_node_t *node, const char *out, char *reason, size_t cap)
{
    airy_artifact_validator_t *av = NULL;
    airy_artifact_meta_t meta = {0};
    airy_artifact_result_t res = {0};

    if (!node->validator_rule_json || !node->validator_rule_json[0])
        return 1;
    if (airy_artifact_validator_from_json(&av, node->validator_rule_json) != AIRY_SUCCESS || !av)
        return 1;
    meta.output = out;
    airy_artifact_validator_validate(av, &meta, &res);
    airy_artifact_validator_destroy(av);
    if (res.verify != AIRY_AV_FAIL)
        return 1;
    snprintf(reason, cap, "%s", res.reason);
    return 0;
}

/* Success branch: an artifact must exist and pass its declared validator before
 * it is taken over as node output. A process-level success without an artifact
 * (NULL, empty or whitespace-only) is not a semantic success: the node is
 * terminal for its dependents while the graph keeps running and converges
 * through sched_dag_propagate_unreachable. */
static int node_succeed(const sched_dag_t *dag, sched_dag_node_t *node, char *out)
{
    char reason[256] = "";

    if (!sched_dag_has_output(out)) {
        node_err_set(node, "agent produced no artifact");
        node_final(node, SCHED_DAG_NODE_SEMANTIC_FAILED);
        SVC_LOG_ERROR("DAG node produced no artifact: %s/%s", dag->dag_id, node->id);
        AIRY_FREE(out);
        return 0;
    }
    if (!node_verify(node, out, reason, sizeof(reason))) {
        node_err_set(node, "artifact verification failed");
        node_final(node, SCHED_DAG_NODE_FAILED);
        SVC_LOG_ERROR("DAG node verification FAILED: %s/%s (%s)", dag->dag_id, node->id, reason);
        AIRY_FREE(out);
        return 0;
    }
    node->output = out;
    node_err_set(node, NULL);
    node_final(node, SCHED_DAG_NODE_COMPLETED);
    SVC_LOG_INFO("DAG node completed: %s/%s (output_len=%zu)", dag->dag_id, node->id,
                 node->output ? strlen(node->output) : 0);
    return 1;
}

/* Whole-graph failure: node FAILED with a reason, graph FAILED, downstream
 * cascade-canceled. Returns the number of canceled nodes. */
static size_t dag_abort(sched_dag_t *dag, sched_dag_node_t *node, const char *msg)
{
    node_err_set(node, msg);
    node->status = SCHED_DAG_NODE_FAILED;
    dag->status = SCHED_DAG_STATUS_FAILED;
    return dag_cascade_cancel(dag);
}

/* Transient failure with retries left and graph budget unspent: back to
 * PENDING with exponential backoff. 1 = retry scheduled. */
static int node_backoff(sched_dag_t *dag, sched_dag_node_t *node, int dret)
{
    uint64_t now;
    uint64_t deadline;
    uint64_t backoff;

    if (!sched_dag_error_is_transient(dret) || node->retry_count >= node->max_retries)
        return 0;
    now = airy_time_wall_ms();
    deadline = dag->retry_budget_ms ? dag->created_at_ms + dag->retry_budget_ms : 0;
    if (deadline != 0 && now >= deadline)
        return 0;
    node->retry_count++;
    backoff = (node->retry_delay_ms ? node->retry_delay_ms : 1000)
              * (1ULL << (node->retry_count - 1));
    node->retry_at_ms = now + backoff;
    node_err_set(node, "transient failure, retrying with backoff");
    node->status = SCHED_DAG_NODE_PENDING;
    node->finished_at_ms = 0;
    SVC_LOG_WARN("DAG node transient failure, retry %u/%u backoff=%llu ms: %s/%s",
                 node->retry_count, node->max_retries, (unsigned long long)backoff, dag->dag_id,
                 node->id);
    return 1;
}

/* Node terminal-state write-back (call with lock held, shared by batch
 * workers; returns whether the node succeeded).
 * Ownership contract: this function fully takes over output (transferred to
 * node->output on success; freed on cancel/failure branches), the caller must
 * not free it again.
 *
 * Outcome grading (improvements 3/4 plus semantic verification):
 *   - graph/node already canceled (or graph FAILED/converged) -> drop result
 *   - success exit code without an artifact -> node SEMANTIC_FAILED, no
 *     cascade: the node is terminal for its dependents but the rest of the
 *     graph keeps running, so the graph can report a partial semantic failure
 *     instead of a hollow success
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
    int fatal;
    const char *why;

    if (node->status == SCHED_DAG_NODE_CANCELED || dag->status == SCHED_DAG_STATUS_CANCELED ||
        dag->status == SCHED_DAG_STATUS_FAILED)
        return node_discard(dag, node, output);

    if (dret == AIRY_SUCCESS)
        return node_succeed(dag, node, output);

    node->finished_at_ms = airy_time_wall_ms();
    node_err_set(node, NULL);
    fatal = svc->dag_fatal_cascade && sched_dag_error_is_fatal(dret);
    why = fatal ? "agent dispatch failed (fatal)"
                : dret == AIRY_ERR_SVC_NOT_READY ? "dag executor not injected"
                                                 : "agent dispatch failed";

    if (fatal || !svc->dag_fatal_cascade) {
        size_t cascade = dag_abort(dag, node, why);
        SVC_LOG_ERROR("DAG node failed: %s/%s (error=%s, rc=%d)", dag->dag_id, node->id,
                      node->error ? node->error : "unknown", dret);
        SVC_LOG_WARN("DAG aborted by node failure%s: %s (%zu nodes cascade-canceled)",
                     fatal ? " (fatal)" : "", dag->dag_id, cascade);
    } else if (!node_backoff(dag, node, dret)) {
        node_err_set(node, "agent dispatch failed");
        node->status = SCHED_DAG_NODE_FAILED;
        SVC_LOG_ERROR("DAG node failed (no cascade): %s/%s (error=%s, rc=%d)", dag->dag_id,
                      node->id, node->error ? node->error : "unknown", dret);
    }
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
                    const sched_dag_node_t *dep = sched_dag_find_node(dag, node->depends[k]);
                    if (!dep || !sched_dag_dep_broken(dep->status))
                        continue;
                    node_final(node, SCHED_DAG_NODE_CANCELED);
                    changed = 1;
                }
            }
        }
    } while (changed);
}

/* Graph terminal-state convergence: finalized once all nodes of every active
 * graph reach a terminal state. Any FAILED node -> graph FAILED (the graph
 * was aborted or a node hard-failed); otherwise any SEMANTIC_FAILED node ->
 * graph SEMANTIC_FAILED (everything ran, at least one node produced no
 * artifact); all COMPLETED -> COMPLETED. Call with lock held. */
void sched_dag_finalize_terminal(sched_service_t *svc)
{
    for (size_t i = 0; i < svc->dag_count; i++) {
        sched_dag_t *dag = svc->dags[i];
        if (dag->status != SCHED_DAG_STATUS_ACTIVE)
            continue;
        if (!sched_dag_all_terminal(svc, dag))
            continue;
        int has_failed = 0;
        int has_semantic = 0;
        for (size_t j = 0; j < dag->node_count; j++) {
            if (dag->nodes[j]->status == SCHED_DAG_NODE_FAILED) {
                has_failed = 1;
                break;
            }
            if (dag->nodes[j]->status == SCHED_DAG_NODE_SEMANTIC_FAILED)
                has_semantic = 1;
        }
        const char *label;
        if (has_failed) {
            dag->status = SCHED_DAG_STATUS_FAILED;
            label = "failed";
        } else if (has_semantic) {
            dag->status = SCHED_DAG_STATUS_SEMANTIC_FAILED;
            label = "semantic_failed";
        } else {
            dag->status = SCHED_DAG_STATUS_COMPLETED;
            label = "completed";
        }
        dag->finished_at_ms = airy_time_wall_ms();
        SVC_LOG_INFO("DAG %s: %s (%zu nodes)", dag->dag_id, label, dag->node_count);
    }
}

/* Smallest retry_at_ms among nodes in backoff wait (PENDING with
 * retry_at_ms > now) across all active graphs; 0 when no node is in backoff.
 * Caller holds the lock. Lets the dag thread wait the shortest backoff
 * instead of busy-polling or blocking forever during backoff periods. */
uint64_t sched_dag_min_retry_at(sched_service_t *svc)
{
    uint64_t min = 0;
    uint64_t now = airy_time_wall_ms();
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
