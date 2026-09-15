// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_dag_internal.h
 * @brief Internal shared declarations of the DAG execution engine
 *        (sched_dag_engine.c) and the DAG worker / parallel batch dispatch
 *        (sched_dag_worker.c).
 * @details sched_dag_impl.c was split by functional domain
 *          (single-responsibility): the core engine helpers below were
 *          promoted from static (formerly in sched_dag_impl.c) because the
 *          worker thread and the parallel batch workers both call them.
 *          For use only by the sched_dag_* translation units.
 */

#ifndef AIRY_RT_SCHED_DAG_INTERNAL_H
#define AIRY_RT_SCHED_DAG_INTERNAL_H

#include "sched_service_internal.h"

/* Total bytes of upstream product a downstream node may receive in addition
 * to its own goal. Split equally across its direct dependencies, so one huge
 * upstream artifact cannot crowd the rest of the graph out of the budget. */
#define SCHED_DAG_UPSTREAM_BUDGET 4096

/* Single source of truth for "this node can no longer change state". The
 * graph convergence check, the progress counters and the dependency gates all
 * consume this predicate, so adding a terminal state stays a one-line change
 * instead of a file-wide audit. */
static inline int sched_dag_node_done(sched_dag_node_status_t st)
{
    return st == SCHED_DAG_NODE_COMPLETED || st == SCHED_DAG_NODE_SEMANTIC_FAILED ||
           st == SCHED_DAG_NODE_FAILED || st == SCHED_DAG_NODE_CANCELED;
}

/* 1 = node terminal and unusable by its dependents (a dependency in this
 * state makes a PENDING node unreachable). */
static inline int sched_dag_dep_broken(sched_dag_node_status_t st)
{
    return st == SCHED_DAG_NODE_SEMANTIC_FAILED || st == SCHED_DAG_NODE_FAILED ||
           st == SCHED_DAG_NODE_CANCELED;
}

/* A dispatched node must come back with an artifact. A success exit code with
 * a NULL, empty or whitespace-only body means the node produced nothing its
 * dependents could consume, so it is not a semantic success. Shared by the
 * write-back path and the parallel dispatcher so "has an artifact" cannot mean
 * two different things. */
static inline int sched_dag_has_output(const char *output)
{
    if (!output)
        return 0;
    for (const char *p = output; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            return 1;
    }
    return 0;
}

/* ---- Engine helpers (defined in sched_dag_engine.c, called by the worker
 *      thread and the parallel batch workers in sched_dag_worker.c) ----
 * All engine helpers expect the caller to hold service->lock. */

/* Agent input for a node: the node goal when it carries real intent, else the
 * graph-level task input (goal==id or empty is only a plan label). */
const char *sched_dag_agent_input(const sched_dag_t *dag, const sched_dag_node_t *node);

/* Compose the effective input of a node: the base input above, followed by the
 * outputs of its direct dependencies that already completed, each capped by an
 * equal share of the byte budget. Without this a multi-node graph has every
 * node start from the bare goal and the intermediate products never connect.
 * Returns a newly allocated string the caller owns (AIRY_FREE); NULL only on
 * allocation failure, in which case the caller falls back to
 * sched_dag_agent_input(). */
char *sched_dag_node_input(const sched_dag_t *dag, const sched_dag_node_t *node,
                           size_t budget);

/* 1 = node ready to dispatch, 0 = deps unresolved, -1 = a dependency is
 * terminal and unusable (node unreachable). */
int sched_dag_node_ready(const sched_dag_t *dag, size_t idx);

/* Scan all active graphs: return the dag index of the first ready node
 * (-1 if none); *out_node receives the node. */
long sched_dag_find_ready(sched_service_t *svc, sched_dag_node_t **out_node);

/* Node terminal-state write-back; takes ownership of output on every path.
 * 1 = node succeeded, 0 = otherwise. */
int sched_dag_write_back_node(sched_service_t *svc, sched_dag_t *dag, sched_dag_node_t *node,
                              int dret, char *output);

/* Mark all PENDING nodes with an unusable (terminal, non-successful)
 * dependency as CANCELED (iterative, until fixpoint), so the graph
 * converges. */
void sched_dag_propagate_unreachable(sched_service_t *svc);

/* Finalize every active graph whose nodes are all terminal:
 * any FAILED node -> graph FAILED, else any SEMANTIC_FAILED node ->
 * graph SEMANTIC_FAILED, else graph COMPLETED. */
void sched_dag_finalize_terminal(sched_service_t *svc);

/* Smallest retry_at_ms among nodes in backoff wait across all active graphs
 * (0 when no node is in backoff). */
uint64_t sched_dag_min_retry_at(sched_service_t *svc);

#endif /* AIRY_RT_SCHED_DAG_INTERNAL_H */
