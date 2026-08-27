// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file sched_service_impl.c
 * @brief Scheduler service core implementation (task-scheduling domain).
 * @details Defines struct sched_service (layout in sched_service_internal.h)
 *          and implements the service lifecycle (create/destroy); the agent
 *          registry/strategy domain lives in sched_service_agent.c, the task
 *          queue/worker domain in sched_service_task.c, and the blueprint
 *          (DAG) scheduling domain in sched_dag_impl.c / sched_dag_engine.c /
 *          sched_dag_worker.c.
 */

#include "scheduler_service.h"
#include "sched_service_internal.h"
#include "svc_logger.h"
#include "platform.h"
#include "thread_pool.h"
#include "multi_agent_collaboration.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cjson/cJSON.h>
#include <airymax/sched.h>

uint64_t sched_now_ms(void)
{
    return (uint64_t)time(NULL) * 1000ull;
}

int sched_service_create(const sched_config_t *config, sched_service_t **service)
{
    if (!config || !service) {
        SVC_LOG_ERROR("sched_service_create: NULL parameter (config=%p, service=%p)",
                      (const void *)config, (const void *)service);
        return AIRY_ERR_INVALID_PARAM;
    }

    sched_service_t *svc = (sched_service_t *)AIRY_CALLOC(1, sizeof(sched_service_t));
    if (!svc) {
        SVC_LOG_ERROR("sched_service_create: calloc failed for service");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    __builtin_memcpy(&svc->config, config, sizeof(sched_config_t));
    if (config->ml_model_path)
        svc->config.ml_model_path = AIRY_STRDUP(config->ml_model_path);

    svc->initialized = 1;
    svc->queue_head = 0;
    svc->queue_tail = 0;
    svc->worker_run = 0;
    svc->worker_thread = AIRY_INVALID_THREAD;
    svc->executor = NULL;
    svc->dag_count = 0;
    svc->dag_seq = 0;
    svc->dag_run = 0;
    svc->dag_thread = AIRY_INVALID_THREAD;
    airy_mtx_init(&svc->lock);
    airy_cond_init(&svc->queue_cond);
    airy_cond_init(&svc->dag_cond);
    airy_mtx_init(&svc->batch_lock);
    airy_cond_init(&svc->batch_cond);
    svc->mac = NULL;
    svc->dag_pool = NULL;
    svc->dag_max_parallel = config->dag_max_parallel;
    svc->dag_batch_size = config->dag_batch_size;
    svc->dag_fatal_cascade = config->dag_fatal_cascade;
    if (svc->dag_batch_size == 0)
        svc->dag_batch_size = svc->dag_max_parallel;
    if (svc->dag_batch_size > SCHED_DAG_MAX_NODES)
        svc->dag_batch_size = SCHED_DAG_MAX_NODES;

    /* Parallel mode: create the delegation framework + concurrent execution
     * thread pool (optional; falls back to serial on failure). Pool
     * min=max=dag_max_parallel: this pool only serves DAG batch concurrency
     * with dag_max_parallel resident workers (thread_pool does not grow
     * dynamically; with min<max, submitted tasks are still consumed serially
     * by the initial threads, so concurrency would not take effect). */
    if (svc->dag_max_parallel > 0) {
        svc->mac = mac_framework_create(MAC_MODE_DELEGATED);
        if (svc->mac) {
            thread_pool_config_t pool_cfg;
            pool_cfg.min_threads = svc->dag_max_parallel;
            pool_cfg.max_threads = svc->dag_max_parallel;
            pool_cfg.queue_size = SCHED_DAG_MAX_NODES;
            pool_cfg.idle_timeout_ms = 30000;
            svc->dag_pool = thread_pool_create(&pool_cfg);
            if (!svc->dag_pool) {
                mac_framework_destroy(svc->mac);
                svc->mac = NULL;
                SVC_LOG_WARN("sched: DAG parallel pool create failed, fallback serial");
            } else {
                SVC_LOG_INFO("sched: DAG parallel mode enabled (max_parallel=%u, batch=%u)",
                             svc->dag_max_parallel, svc->dag_batch_size);
            }
        } else {
            SVC_LOG_WARN("sched: mac_framework create failed, fallback serial");
        }
    }

    *service = svc;
    return 0;
}

int sched_service_destroy(sched_service_t *service)
{
    if (!service) {
        SVC_LOG_ERROR("sched_service_destroy: NULL service parameter");
        return AIRY_ERR_INVALID_PARAM;
    }

    sched_service_stop_workers(service);

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (service->agents[i]) {
            AIRY_FREE(service->agents[i]->agent_id);
            AIRY_FREE(service->agents[i]->agent_name);
            AIRY_FREE(service->agents[i]);
        }
    }
    for (size_t i = 0; i < service->task_count; i++) {
        task_record_t *rec = service->tasks[i];
        AIRY_FREE(rec->task_id);
        AIRY_FREE(rec->task_description);
        AIRY_FREE(rec->selected_agent_id);
        AIRY_FREE(rec->output);
        AIRY_FREE(rec->error);
        AIRY_FREE(rec);
    }
    service->task_count = 0;
    for (size_t i = 0; i < service->dag_count; i++) {
        sched_dag_t *dag = service->dags[i];
        for (size_t j = 0; j < dag->node_count; j++) {
            sched_dag_node_t *node = dag->nodes[j];
            AIRY_FREE(node->id);
            AIRY_FREE(node->goal);
            AIRY_FREE(node->role);
            AIRY_FREE(node->validator_rule_json);
            for (size_t k = 0; k < node->dep_count; k++) {
                AIRY_FREE(node->depends[k]);
            }
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
    service->dag_count = 0;
    airy_mtx_unlock(&service->lock);

    if (service->dag_pool) {
        thread_pool_destroy(service->dag_pool);
        service->dag_pool = NULL;
    }
    if (service->mac) {
        mac_framework_destroy(service->mac);
        service->mac = NULL;
    }

    AIRY_FREE((void *)service->config.ml_model_path);
    airy_cond_destroy(&service->batch_cond);
    airy_mtx_destroy(&service->batch_lock);
    airy_cond_destroy(&service->dag_cond);
    airy_cond_destroy(&service->queue_cond);
    airy_mtx_destroy(&service->lock);
    AIRY_FREE(service);
    return 0;
}
