// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_dag_impl.c
 * @brief Scheduler service - blueprint scheduling (DAG) public APIs domain.
 * @details Implements the DAG public APIs in scheduler_service.h
 *          (submit_dag/get_dag/list_dags/cancel_dag/checkpoint_save). Moved out of the
 *          original monolithic file by functional domain: the execution
 *          engine (dependency resolution, failure tiers, retry backpressure,
 *          topological convergence) lives in sched_dag_engine.c, the worker
 *          thread / parallel batch dispatch in sched_dag_worker.c, and JSON
 *          parsing/validation in sched_dag_parse.c.
 */

#include "sched_service_internal.h"
#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <cjson/cJSON.h>

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
        AIRY_FREE(dag->input);
        AIRY_FREE(dag->workspace_dir);
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
        AIRY_FREE(dag->input);
        AIRY_FREE(dag->workspace_dir);
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

int sched_service_list_dags(sched_service_t *service, char **out_json)
{
    if (!service || !out_json || !service->initialized) {
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_json = NULL;

    static const char *dag_status_names[] = {"active", "completed", "failed", "canceled"};

    airy_mtx_lock(&service->lock);
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON *dags = cJSON_CreateArray();
        for (size_t i = 0; i < service->dag_count; i++) {
            sched_dag_t *dag = service->dags[i];
            cJSON *dj = cJSON_CreateObject();
            if (!dj)
                continue;
            cJSON_AddStringToObject(dj, "dag_id", dag->dag_id);
            cJSON_AddStringToObject(dj, "name", dag->name ? dag->name : "");
            cJSON_AddStringToObject(dj, "status",
                                    dag_status_names[dag->status % SCHED_DAG_STATUS_COUNT]);
            cJSON_AddNumberToObject(dj, "node_count", (double)dag->node_count);
            size_t done = 0;
            for (size_t j = 0; j < dag->node_count; j++) {
                sched_dag_node_status_t st = dag->nodes[j]->status;
                if (st == SCHED_DAG_NODE_COMPLETED || st == SCHED_DAG_NODE_FAILED ||
                    st == SCHED_DAG_NODE_CANCELED)
                    done++;
            }
            cJSON_AddNumberToObject(dj, "progress", (double)done);
            cJSON_AddNumberToObject(dj, "created_at_ms", (double)dag->created_at_ms);
            cJSON_AddNumberToObject(dj, "finished_at_ms", (double)dag->finished_at_ms);
            cJSON_AddItemToArray(dags, dj);
        }
        cJSON_AddItemToObject(root, "dags", dags);
        cJSON_AddNumberToObject(root, "count", (double)service->dag_count);
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
