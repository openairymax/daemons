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

#include "sched_service_internal.h"
#include "airy_memory.h"
#include "error.h"

#include <string.h>
#include <cjson/cJSON.h>

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

    cJSON *name = cJSON_GetObjectItem(root, "name");
    dag->name =
        AIRY_STRDUP(cJSON_IsString(name) && name->valuestring ? name->valuestring : "unnamed_dag");
    /* Graded retry (improvement 4): graph-level shared retry budget window ms
     * (0 = unlimited). The total retry deadline for all nodes is
     * created_at + retry_budget_ms; exceeding the budget stops retries. */
    cJSON *rbudget = cJSON_GetObjectItem(root, "retry_budget_ms");
    if (cJSON_IsNumber(rbudget) && rbudget->valuedouble > 0)
        dag->retry_budget_ms = (uint64_t)rbudget->valuedouble;
    dag->node_count = 0;

    /* Nodes: id required and unique; goal defaults ""; role defaults "coding";
     * depends array. err records the concrete failure reason: structurally
     * invalid INVALID_PARAM / alloc failure OOM / dependency cycle
     * CYCLE_DETECTED (previously every failure was lumped into cycle
     * detection, misleading callers). */
    int err = AIRY_SUCCESS;
    for (int i = 0; i < n; i++) {
        cJSON *nj = cJSON_GetArrayItem(nodes_json, i);
        cJSON *nid = cJSON_GetObjectItem(nj, "id");
        if (!cJSON_IsString(nid) || !nid->valuestring || !nid->valuestring[0]) {
            err = AIRY_ERR_INVALID_PARAM;
            break;
        }
        for (size_t p = 0; p < dag->node_count; p++) {
            if (strcmp(dag->nodes[p]->id, nid->valuestring) == 0) {
                err = AIRY_ERR_INVALID_PARAM;
                break;
            }
        }
        if (err != AIRY_SUCCESS)
            break;

        sched_dag_node_t *node = (sched_dag_node_t *)AIRY_CALLOC(1, sizeof(sched_dag_node_t));
        if (!node) {
            err = AIRY_ERR_OUT_OF_MEMORY;
            break;
        }
        node->id = AIRY_STRDUP(nid->valuestring);
        cJSON *goal = cJSON_GetObjectItem(nj, "goal");
        node->goal =
            AIRY_STRDUP(cJSON_IsString(goal) && goal->valuestring ? goal->valuestring : "");
        cJSON *role = cJSON_GetObjectItem(nj, "role");
        node->role =
            AIRY_STRDUP(cJSON_IsString(role) && role->valuestring ? role->valuestring : "coding");

        cJSON *mretry = cJSON_GetObjectItem(nj, "max_retries");
        if (cJSON_IsNumber(mretry) && mretry->valuedouble > 0) {
            node->max_retries = (uint32_t)mretry->valuedouble;
            cJSON *rdelay = cJSON_GetObjectItem(nj, "retry_delay_ms");
            if (cJSON_IsNumber(rdelay) && rdelay->valuedouble > 0)
                node->retry_delay_ms = (uint32_t)rdelay->valuedouble;
        }

        cJSON *validator = cJSON_GetObjectItem(nj, "validator");
        if (cJSON_IsObject(validator)) {
            node->validator_rule_json = cJSON_PrintUnformatted(validator);
            if (!node->validator_rule_json)
                err = AIRY_ERR_OUT_OF_MEMORY;
        }
        if (!node->id || !node->goal || !node->role) {
            err = AIRY_ERR_OUT_OF_MEMORY;
        }
        cJSON *deps = cJSON_GetObjectItem(nj, "depends");
        if (cJSON_IsArray(deps) && err == AIRY_SUCCESS) {
            int dn = cJSON_GetArraySize(deps);
            if (dn > SCHED_DAG_MAX_DEPS) {
                err = AIRY_ERR_INVALID_PARAM;
            }
            for (int k = 0; k < dn && err == AIRY_SUCCESS; k++) {
                cJSON *dj = cJSON_GetArrayItem(deps, k);
                if (!cJSON_IsString(dj) || !dj->valuestring) {
                    err = AIRY_ERR_INVALID_PARAM;
                    break;
                }
                /* Dependencies must point to nodes that already exist in the
                 * graph (declared before or after the node, so first collect
                 * all dependencies, then validate existence in a second pass) */
                node->depends[node->dep_count] = AIRY_STRDUP(dj->valuestring);
                if (!node->depends[node->dep_count]) {
                    err = AIRY_ERR_OUT_OF_MEMORY;
                    break;
                }
                node->dep_count++;
            }
        }
        dag->nodes[dag->node_count++] = node;
        if (err != AIRY_SUCCESS)
            break;
    }

    for (size_t i = 0; i < dag->node_count && err == AIRY_SUCCESS; i++) {
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
                err = AIRY_ERR_INVALID_PARAM;
                break;
            }
        }
    }

    if (err == AIRY_SUCCESS) {
        size_t *indeg = (size_t *)AIRY_CALLOC(dag->node_count, sizeof(size_t));
        if (!indeg) {
            err = AIRY_ERR_OUT_OF_MEMORY;
        } else {
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
                err = AIRY_ERR_CYCLE_DETECTED;
        }
    }

    if (err != AIRY_SUCCESS) {

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
        AIRY_FREE(dag->dag_id);
        AIRY_FREE(dag->name);
        AIRY_FREE(dag);
        return err;
    }

    *out_dag = dag;
    return AIRY_SUCCESS;
}
