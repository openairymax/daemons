// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_dag_parse.c
 * @brief Scheduler service - blueprint scheduling (DAG) JSON parsing and
 *        topological-validation domain.
 * @details Parses nodes/dependencies/retry/validation rules from the
 *          blueprint-submission JSON and builds the sched_dag_t internal
 *          structure; handles uniqueness checks, dependency-existence checks
 *          and cycle detection (Kahn topological sort), fully rolling back
 *          allocated memory on failure.
 */

#include "sched_dag_internal.h"
#include "airy_memory.h"
#include "error.h"
#include "agent_vocab.h"

#include <string.h>
#include <cjson/cJSON.h>

static void node_fields(sched_dag_node_t *node, cJSON *nj, int *err)
{
    cJSON *goal = cJSON_GetObjectItem(nj, "goal");
    node->goal =
        AIRY_STRDUP(cJSON_IsString(goal) && goal->valuestring ? goal->valuestring : "");
    cJSON *role = cJSON_GetObjectItem(nj, "role");
    node->role = AIRY_STRDUP(cJSON_IsString(role) && role->valuestring ? role->valuestring
                                                                       : AGENT_VOCAB_FALLBACK);

    cJSON *mretry = cJSON_GetObjectItem(nj, "max_retries");
    if (cJSON_IsNumber(mretry) && mretry->valuedouble > 0) {
        node->max_retries = (uint32_t)mretry->valuedouble;
        cJSON *rdelay = cJSON_GetObjectItem(nj, "retry_delay_ms");
        if (cJSON_IsNumber(rdelay) && rdelay->valuedouble > 0)
            node->retry_delay_ms = (uint32_t)rdelay->valuedouble;
    }

    cJSON *timeout = cJSON_GetObjectItem(nj, "timeout_ms");
    if (cJSON_IsNumber(timeout) && timeout->valuedouble > 0)
        node->exec_timeout_ms = (uint32_t)timeout->valuedouble;

    cJSON *validator = cJSON_GetObjectItem(nj, "validator");
    if (cJSON_IsObject(validator)) {
        node->validator_rule_json = cJSON_PrintUnformatted(validator);
        if (!node->validator_rule_json)
            *err = AIRY_ERR_OUT_OF_MEMORY;
    }
    if (!node->id || !node->goal || !node->role)
        *err = AIRY_ERR_OUT_OF_MEMORY;
}

static void node_deps(sched_dag_node_t *node, cJSON *nj, int *err)
{
    cJSON *deps = cJSON_GetObjectItem(nj, "depends");
    if (cJSON_IsArray(deps) && *err == AIRY_SUCCESS) {
        int dn = cJSON_GetArraySize(deps);
        if (dn > SCHED_DAG_MAX_DEPS)
            *err = AIRY_ERR_INVALID_PARAM;
        for (int k = 0; k < dn && *err == AIRY_SUCCESS; k++) {
            cJSON *dj = cJSON_GetArrayItem(deps, k);
            if (!cJSON_IsString(dj) || !dj->valuestring) {
                *err = AIRY_ERR_INVALID_PARAM;
                break;
            }
            node->depends[node->dep_count] = AIRY_STRDUP(dj->valuestring);
            if (!node->depends[node->dep_count]) {
                *err = AIRY_ERR_OUT_OF_MEMORY;
                break;
            }
            node->dep_count++;
        }
    }
}

/* Second pass: every dependency must point to an existing node in the graph. */
static void sched_dag_validate_deps(sched_dag_t *dag, int *err)
{
    for (size_t i = 0; i < dag->node_count && *err == AIRY_SUCCESS; i++) {
        sched_dag_node_t *node = dag->nodes[i];
        for (size_t k = 0; k < node->dep_count; k++) {
            int hit = 0;
            for (size_t j = 0; j < dag->node_count; j++) {
                if (strcmp(dag->nodes[j]->id, node->depends[k]) == 0) {
                    hit = 1;
                    break;
                }
            }
            if (!hit) {
                *err = AIRY_ERR_INVALID_PARAM;
                break;
            }
        }
    }
}

/* Kahn topological sort: nodes whose dependencies are all processed get
 * marked; if not every node is processed a dependency cycle exists. */
static void sched_dag_detect_cycle(sched_dag_t *dag, int *err)
{
    size_t *indeg = (size_t *)AIRY_CALLOC(dag->node_count, sizeof(size_t));
    if (!indeg) {
        *err = AIRY_ERR_OUT_OF_MEMORY;
        return;
    }

    for (size_t i = 0; i < dag->node_count; i++) {
        for (size_t k = 0; k < dag->nodes[i]->dep_count; k++) {
            for (size_t j = 0; j < dag->node_count; j++) {
                if (strcmp(dag->nodes[j]->id, dag->nodes[i]->depends[k]) == 0) {
                    indeg[i]++;
                    break;
                }
            }
        }
    }

    size_t processed = 0;
    int progressed = 1;
    while (progressed) {
        progressed = 0;
        for (size_t i = 0; i < dag->node_count; i++) {
            if (indeg[i] == SIZE_MAX)
                continue;
            int all_ok = 1;
            for (size_t k = 0; k < dag->nodes[i]->dep_count && all_ok; k++) {
                for (size_t j = 0; j < dag->node_count; j++) {
                    if (strcmp(dag->nodes[j]->id, dag->nodes[i]->depends[k]) == 0) {
                        if (indeg[j] != SIZE_MAX)
                            all_ok = 0;
                        break;
                    }
                }
            }
            if (all_ok) {
                indeg[i] = SIZE_MAX;
                processed++;
                progressed = 1;
            }
        }
    }
    AIRY_FREE(indeg);
    if (processed != dag->node_count)
        *err = AIRY_ERR_CYCLE_DETECTED;
}

void sched_dag_free(sched_dag_t *dag)
{
    if (!dag)
        return;
    for (size_t i = 0; i < dag->node_count; i++) {
        sched_dag_node_t *node = dag->nodes[i];
        AIRY_FREE(node->id);
        AIRY_FREE(node->goal);
        AIRY_FREE(node->role);
        AIRY_FREE(node->validator_rule_json);
        for (size_t k = 0; k < node->dep_count; k++)
            AIRY_FREE(node->depends[k]);
        AIRY_FREE(node->output);
        AIRY_FREE(node->error);
        AIRY_FREE(node);
    }
    AIRY_FREE(dag->dag_id);
    AIRY_FREE(dag->name);
    AIRY_FREE(dag->input);
    AIRY_FREE(dag->workspace_dir);
    AIRY_FREE(dag);
}

/* Graph-level scalars: name/input/workspace defaults, shared retry window,
 * and the per-node default execution budget (node timeout_ms overrides it). */
static void dag_meta(sched_dag_t *dag, cJSON *root)
{
    cJSON *name = cJSON_GetObjectItem(root, "name");
    dag->name =
        AIRY_STRDUP(cJSON_IsString(name) && name->valuestring ? name->valuestring : "unnamed_dag");
    cJSON *input = cJSON_GetObjectItem(root, "input");
    dag->input =
        AIRY_STRDUP(cJSON_IsString(input) && input->valuestring ? input->valuestring : "");
    cJSON *ws = cJSON_GetObjectItem(root, "workspace_dir");
    dag->workspace_dir =
        AIRY_STRDUP(cJSON_IsString(ws) && ws->valuestring ? ws->valuestring : "");
    /* Graded retry (improvement 4): graph-level shared retry budget window ms
     * (0 = unlimited). The total retry deadline for all nodes is
     * created_at + retry_budget_ms; exceeding the budget stops retries. */
    cJSON *rbudget = cJSON_GetObjectItem(root, "retry_budget_ms");
    if (cJSON_IsNumber(rbudget) && rbudget->valuedouble > 0)
        dag->retry_budget_ms = (uint64_t)rbudget->valuedouble;
    /* Graph-level default execution budget per node (0 = dispatch default);
     * a node-level timeout_ms overrides it. Propagated to the executor so a
     * hung agent ends in a terminal TIMEOUT instead of a stuck RUNNING node. */
    cJSON *dtimeout = cJSON_GetObjectItem(root, "timeout_ms");
    if (cJSON_IsNumber(dtimeout) && dtimeout->valuedouble > 0)
        dag->exec_timeout_ms = (uint32_t)dtimeout->valuedouble;
}

/* One node: id required and unique; goal/role default; depends array. The
 * node stays owned by the dag (counted) even on partial failure so
 * sched_dag_free reclaims it. */
static void dag_add_node(sched_dag_t *dag, cJSON *nj, int *err)
{
    cJSON *nid = cJSON_GetObjectItem(nj, "id");
    if (!cJSON_IsString(nid) || !nid->valuestring || !nid->valuestring[0]) {
        *err = AIRY_ERR_INVALID_PARAM;
        return;
    }
    for (size_t p = 0; p < dag->node_count; p++) {
        if (strcmp(dag->nodes[p]->id, nid->valuestring) == 0) {
            *err = AIRY_ERR_INVALID_PARAM;
            return;
        }
    }

    sched_dag_node_t *node = (sched_dag_node_t *)AIRY_CALLOC(1, sizeof(sched_dag_node_t));
    if (!node) {
        *err = AIRY_ERR_OUT_OF_MEMORY;
        return;
    }
    node->id = AIRY_STRDUP(nid->valuestring);
    node_fields(node, nj, err);
    node_deps(node, nj, err);
    dag->nodes[dag->node_count++] = node;
}

int sched_dag_validate_and_build(cJSON *root, sched_dag_t **out_dag)
{
    cJSON *nodes_json = cJSON_GetObjectItem(root, "nodes");
    if (!cJSON_IsArray(nodes_json) || cJSON_GetArraySize(nodes_json) == 0) {
        return AIRY_ERR_INVALID_PARAM;
    }
    int n = cJSON_GetArraySize(nodes_json);
    if (n > SCHED_DAG_MAX_NODES) {
        return AIRY_ERR_OVERFLOW;
    }

    sched_dag_t *dag = (sched_dag_t *)AIRY_CALLOC(1, sizeof(sched_dag_t));
    if (!dag)
        return AIRY_ERR_OUT_OF_MEMORY;

    dag_meta(dag, root);
    dag->node_count = 0;

    /* err records the concrete failure reason: structurally invalid
     * INVALID_PARAM / alloc failure OOM / dependency cycle CYCLE_DETECTED
     * (previously every failure was lumped into cycle detection, misleading
     * callers). */
    int err = AIRY_SUCCESS;
    for (int i = 0; i < n && err == AIRY_SUCCESS; i++)
        dag_add_node(dag, cJSON_GetArrayItem(nodes_json, i), &err);

    if (err == AIRY_SUCCESS)
        sched_dag_validate_deps(dag, &err);
    if (err == AIRY_SUCCESS)
        sched_dag_detect_cycle(dag, &err);

    if (err != AIRY_SUCCESS) {
        sched_dag_free(dag);
        return err;
    }

    *out_dag = dag;
    return AIRY_SUCCESS;
}
