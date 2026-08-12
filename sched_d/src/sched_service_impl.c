// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file sched_service_impl.c
 * @brief 调度服务核心实现（任务调度域）
 * @details 定义 struct sched_service（布局见 sched_service_internal.h）并实现
 *          scheduler_service.h 中任务调度域公共 API（队列/选路/worker 线程）；
 *          蓝图调度（DAG）域见 sched_dag_impl.c
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

/* Sync an agent to mac_framework (parallel delegation mode).
 * Field mapping: id/name truncated into fixed arrays; performance_score=weight
 * (load balancing), reliability_score=success_rate;
 * max_concurrent_tasks=dag_max_parallel (propagating the sched parallelism
 * config to mac selection throttling); available=is_available.
 * Caller must already hold service->lock (inner mac lock is nested, consistent
 * lock order, no deadlock). mac has no update semantics: unregister first
 * (ignore not found), then register, to implement refresh. */
static void sched_mac_sync_agent(sched_service_t *service, const agent_info_t *agent)
{
    if (!service->mac || !agent || !agent->agent_id)
        return;

    mac_agent_info_t ma;
    __builtin_memset(&ma, 0, sizeof(ma));
    AIRY_STRNCPY_TERM(ma.id, agent->agent_id, sizeof(ma.id));
    ma.id[sizeof(ma.id) - 1] = '\0';
    if (agent->agent_name) {
        AIRY_STRNCPY_TERM(ma.name, agent->agent_name, sizeof(ma.name));
        ma.name[sizeof(ma.name) - 1] = '\0';
    }
    ma.performance_score = agent->weight;
    ma.reliability_score = agent->success_rate;
    ma.max_concurrent_tasks = (int)service->dag_max_parallel;
    ma.available = agent->is_available;
    ma.capabilities_json = NULL;

    mac_framework_unregister_agent(service->mac, ma.id);
    if (mac_framework_register_agent(service->mac, &ma) != 0) {
        SVC_LOG_WARN("sched: mac register agent failed: %s", ma.id);
    }
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

int sched_service_register_agent(sched_service_t *service, const agent_info_t *agent_info)
{
    if (!service || !agent_info || !service->initialized) {
        SVC_LOG_ERROR("sched_service_register_agent: NULL parameter or not initialized "
                      "(service=%p, agent_info=%p, initialized=%d)",
                      (const void *)service, (const void *)agent_info,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    if (service->agent_count >= AIRY_CAP_MAX_AGENTS) {
        SVC_LOG_ERROR("sched_service_register_agent: max agents exceeded (count=%zu, max=%d)",
                      service->agent_count, AIRY_CAP_MAX_AGENTS);
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_OVERFLOW;
    }

    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, agent_info->agent_id) == 0) {
            service->agents[i]->load_factor = agent_info->load_factor;
            service->agents[i]->success_rate = agent_info->success_rate;
            service->agents[i]->avg_response_time_ms = agent_info->avg_response_time_ms;
            service->agents[i]->is_available = agent_info->is_available;
            service->agents[i]->weight = agent_info->weight;
            sched_mac_sync_agent(service, service->agents[i]);
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }

    agent_info_t *new_agent = (agent_info_t *)AIRY_CALLOC(1, sizeof(agent_info_t));
    if (!new_agent) {
        SVC_LOG_ERROR("sched_service_register_agent: calloc failed for new agent");
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    if (!agent_info->agent_id) {
        SVC_LOG_ERROR("sched_service_register_agent: agent_info->agent_id is NULL");
        AIRY_FREE(new_agent);
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_INVALID_PARAM;
    }
    new_agent->agent_id = AIRY_STRDUP(agent_info->agent_id);
    new_agent->agent_name =
        agent_info->agent_name ? AIRY_STRDUP(agent_info->agent_name) : AIRY_STRDUP("");
    if (!new_agent->agent_id || !new_agent->agent_name) {
        SVC_LOG_ERROR("sched_service_register_agent: strdup failed for agent fields (agent_id=%p, "
                      "agent_name=%p)",
                      (const void *)new_agent->agent_id, (const void *)new_agent->agent_name);
        AIRY_FREE(new_agent->agent_id);
        AIRY_FREE(new_agent->agent_name);
        AIRY_FREE(new_agent);
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    new_agent->load_factor = agent_info->load_factor;
    new_agent->success_rate = agent_info->success_rate;
    new_agent->avg_response_time_ms = agent_info->avg_response_time_ms;
    new_agent->is_available = agent_info->is_available;
    new_agent->weight = agent_info->weight;

    service->agents[service->agent_count++] = new_agent;
    SVC_LOG_INFO("sched: agent registered: id=%s name=%s avail=%d weight=%.2f "
                 "success_rate=%.2f load=%.2f (total_agents=%zu)",
                 new_agent->agent_id, new_agent->agent_name, new_agent->is_available,
                 (double)new_agent->weight, (double)new_agent->success_rate,
                 (double)new_agent->load_factor, service->agent_count);
    sched_mac_sync_agent(service, new_agent);
    airy_mtx_unlock(&service->lock);
    return 0;
}

int sched_service_unregister_agent(sched_service_t *service, const char *agent_id)
{
    if (!service || !agent_id || !service->initialized) {
        SVC_LOG_ERROR("sched_service_unregister_agent: NULL parameter or not initialized "
                      "(service=%p, agent_id=%p, initialized=%d)",
                      (const void *)service, (const void *)agent_id,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, agent_id) == 0) {
            if (service->mac) {

                mac_framework_unregister_agent(service->mac, agent_id);
            }
            AIRY_FREE(service->agents[i]->agent_id);
            AIRY_FREE(service->agents[i]->agent_name);
            AIRY_FREE(service->agents[i]);

            for (size_t j = i; j < service->agent_count - 1; j++) {
                service->agents[j] = service->agents[j + 1];
            }
            service->agent_count--;
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }
    airy_mtx_unlock(&service->lock);
    SVC_LOG_ERROR("sched_service_unregister_agent: agent not found (agent_id=%s)",
                  agent_id ? agent_id : "NULL");
    return AIRY_ERR_NOT_FOUND;
}

int sched_service_update_agent_status(sched_service_t *service, const agent_info_t *agent_info)
{
    if (!service || !agent_info || !service->initialized) {
        SVC_LOG_ERROR("sched_service_update_agent_status: NULL parameter or not initialized "
                      "(service=%p, agent_info=%p, initialized=%d)",
                      (const void *)service, (const void *)agent_info,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    for (size_t i = 0; i < service->agent_count; i++) {
        if (strcmp(service->agents[i]->agent_id, agent_info->agent_id) == 0) {
            service->agents[i]->load_factor = agent_info->load_factor;
            service->agents[i]->success_rate = agent_info->success_rate;
            service->agents[i]->avg_response_time_ms = agent_info->avg_response_time_ms;
            service->agents[i]->is_available = agent_info->is_available;
            service->agents[i]->weight = agent_info->weight;
            sched_mac_sync_agent(service, service->agents[i]);
            airy_mtx_unlock(&service->lock);
            return 0;
        }
    }
    airy_mtx_unlock(&service->lock);
    SVC_LOG_ERROR("sched_service_update_agent_status: agent not found (agent_id=%s)",
                  agent_info->agent_id ? agent_info->agent_id : "NULL");
    return AIRY_ERR_NOT_FOUND;
}

int sched_service_schedule_task(sched_service_t *service, const task_info_t *task_info,
                                sched_result_t **result)
{
    if (!service || !task_info || !result || !service->initialized) {
        SVC_LOG_ERROR("sched_service_schedule_task: NULL parameter or not initialized (service=%p, "
                      "task_info=%p, result=%p, initialized=%d)",
                      (const void *)service, (const void *)task_info, (const void *)result,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    sched_result_t *res = (sched_result_t *)AIRY_CALLOC(1, sizeof(sched_result_t));
    if (!res) {
        SVC_LOG_ERROR("sched_service_schedule_task: calloc failed for result");
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    agent_info_t *best_agent = NULL;
    float best_score = -1.0f;

    for (size_t i = 0; i < service->agent_count; i++) {
        if (!service->agents[i]->is_available)
            continue;

        float score = 0.0f;
        switch (service->config.strategy) {
        case SCHED_STRATEGY_WEIGHTED:
            score = service->agents[i]->weight * service->agents[i]->success_rate *
                    (1.0f - service->agents[i]->load_factor);
            break;
        case SCHED_STRATEGY_ROUND_ROBIN:
            if (service->agent_count == 0) {
                score = 0.0f;
            } else {
                score = (float)(service->total_tasks_scheduled % service->agent_count);
                if ((size_t)score == i)
                    score = 100.0f;
                else
                    score = 0.0f;
            }
            break;
        case SCHED_STRATEGY_ML_BASED:
            score = service->agents[i]->success_rate * (1.0f - service->agents[i]->load_factor);
            break;
        case SCHED_STRATEGY_PRIORITY_BASED: {
            /* Priority strategy: amplify the selection score by task priority
             * on top of weighting (semantics aligned with priority_weight in
             * strategies/priority_based.c: the more urgent the task, the more
             * it prefers healthy, low-load agents). */
            float pw = 1.0f;
            if (task_info->priority >= TASK_PRIORITY_URGENT)
                pw = 4.0f;
            else if (task_info->priority >= TASK_PRIORITY_HIGH)
                pw = 2.0f;
            else if (task_info->priority >= TASK_PRIORITY_NORMAL)
                pw = 1.5f;
            score = (service->agents[i]->weight * service->agents[i]->success_rate *
                     (1.0f - service->agents[i]->load_factor)) *
                    pw;
            break;
        }
        default:
            score = service->agents[i]->weight;
            break;
        }

        if (score > best_score) {
            best_score = score;
            best_agent = service->agents[i];
        }
    }

    service->total_tasks_scheduled++;

    if (best_agent) {
        res->selected_agent_id = AIRY_STRDUP(best_agent->agent_id);
        res->confidence = best_score > 0 ? (best_score > 1.0f ? 1.0f : best_score) : 0.5f;
        res->estimated_time_ms = best_agent->avg_response_time_ms;
        service->total_success++;
        SVC_LOG_DEBUG("sched: agent selected: task=%s agent=%s score=%.3f "
                      "confidence=%.2f strategy=%d candidates=%zu (avail filtered)",
                      task_info->task_id ? task_info->task_id : "?", best_agent->agent_id,
                      (double)best_score, (double)res->confidence, (int)service->config.strategy,
                      service->agent_count);
    } else {
        SVC_LOG_ERROR(
            "sched_service_schedule_task: no available agent found (agent_count=%zu, strategy=%d)",
            service->agent_count, service->config.strategy);
        res->selected_agent_id = NULL;
        res->confidence = 0.0f;
        res->estimated_time_ms = 0;
    }

    *result = res;
    airy_mtx_unlock(&service->lock);
    return 0;
}

int sched_service_get_stats(sched_service_t *service, void **stats)
{
    if (!service || !stats || !service->initialized) {
        SVC_LOG_ERROR("sched_service_get_stats: NULL parameter or not initialized (service=%p, "
                      "stats=%p, initialized=%d)",
                      (const void *)service, (const void *)stats,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    char *json_stats = (char *)AIRY_MALLOC(512);
    if (!json_stats) {
        SVC_LOG_ERROR("sched_service_get_stats: malloc failed for stats JSON");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    snprintf(json_stats, 512,
             "{\"agent_count\":%zu,\"total_tasks\":%llu,\"success_rate\":\"%.2f\",\"strategy\":%d}",
             service->agent_count, (unsigned long long)service->total_tasks_scheduled,
             service->total_tasks_scheduled > 0 ?
                 (float)service->total_success / (float)service->total_tasks_scheduled :
                 0.0f,
             service->config.strategy);

    *stats = json_stats;
    return 0;
}

int sched_service_health_check(sched_service_t *service, bool *health_status)
{
    if (!service || !health_status || !service->initialized) {
        SVC_LOG_ERROR("sched_service_health_check: NULL parameter or not initialized (service=%p, "
                      "health_status=%p, initialized=%d)",
                      (const void *)service, (const void *)health_status,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    bool all_healthy = true;
    for (size_t i = 0; i < service->agent_count; i++) {
        if (service->agents[i]->load_factor > 0.95f) {
            all_healthy = false;
            break;
        }
    }

    *health_status = all_healthy && service->initialized;
    return 0;
}

int sched_service_reload_config(sched_service_t *service, const sched_config_t *config)
{
    if (!service || !config || !service->initialized) {
        SVC_LOG_ERROR("sched_service_reload_config: NULL parameter or not initialized (service=%p, "
                      "config=%p, initialized=%d)",
                      (const void *)service, (const void *)config,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }

    AIRY_FREE((void *)service->config.ml_model_path);
    service->config.ml_model_path = NULL;

    __builtin_memcpy(&service->config, config, sizeof(sched_config_t));
    if (config->ml_model_path) {
        service->config.ml_model_path = AIRY_STRDUP(config->ml_model_path);
        if (!service->config.ml_model_path) {
            SVC_LOG_ERROR("sched_service_reload_config: strdup failed for ml_model_path (path=%s)",
                          config->ml_model_path ? config->ml_model_path : "NULL");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
    }

    return 0;
}

/*
 * Pick and remove the highest-priority task from the ring queue (FIFO among
 * equal priorities, taking the earliest enqueued). Caller must hold the lock;
 * returns NULL when the queue is empty. Compact removal keeps the ring
 * semantics hole-free (consistent with cancel_task's queue compaction).
 */
static task_record_t *sched_queue_take_highest(sched_service_t *svc)
{
    if (svc->queue_head == svc->queue_tail)
        return NULL;

    size_t best = svc->queue_head;
    task_priority_t best_p = svc->queue[best]->priority;
    for (size_t qi = svc->queue_head; qi != svc->queue_tail; qi = (qi + 1) % AIRY_CAP_MAX_TASKS) {
        if (svc->queue[qi]->priority > best_p) {
            best = qi;
            best_p = svc->queue[qi]->priority;
        }
    }
    task_record_t *rec = svc->queue[best];

    size_t src = (best + 1) % AIRY_CAP_MAX_TASKS;
    size_t dst = best;
    while (src != svc->queue_tail) {
        svc->queue[dst] = svc->queue[src];
        src = (src + 1) % AIRY_CAP_MAX_TASKS;
        dst = (dst + 1) % AIRY_CAP_MAX_TASKS;
    }
    svc->queue_tail = (svc->queue_tail + AIRY_CAP_MAX_TASKS - 1) % AIRY_CAP_MAX_TASKS;
    size_t remain = (svc->queue_tail + AIRY_CAP_MAX_TASKS - svc->queue_head) % AIRY_CAP_MAX_TASKS;
    SVC_LOG_DEBUG("sched: queue take: task=%s priority=%d wait_ms=%llu remain=%zu", rec->task_id,
                  (int)rec->priority, (unsigned long long)(sched_now_ms() - rec->created_at_ms),
                  remain);
    return rec;
}

/*
 * Worker thread: consumes the pending queue by priority (highest first, FIFO
 * within the same priority), running select agent -> executor (spawn+invoke) ->
 * status write-back. The lock is held only for queue operations/status
 * write-back; selection and dispatch (time-consuming) run outside the lock so
 * long tasks do not block register_agent and other RPCs.
 */
static void *sched_worker_thread(void *arg)
{
    sched_service_t *svc = (sched_service_t *)arg;

    while (1) {
        airy_mtx_lock(&svc->lock);

        while (svc->queue_head == svc->queue_tail && svc->worker_run) {
            SVC_LOG_DEBUG("sched: worker idle, waiting for tasks");
            airy_cond_wait(&svc->queue_cond, &svc->lock);
        }
        if (!svc->worker_run) {
            SVC_LOG_INFO("sched: worker received stop signal, exiting");
            airy_mtx_unlock(&svc->lock);
            break;
        }
        task_record_t *rec = sched_queue_take_highest(svc);
        if (!rec) {
            SVC_LOG_WARN("sched: worker woke but queue empty (spurious wakeup?)");
            airy_mtx_unlock(&svc->lock);
            continue;
        }
        rec->status = SCHED_TASK_STATUS_RUNNING;
        const uint64_t wait_ms = sched_now_ms() - rec->created_at_ms;
        SVC_LOG_INFO("sched: task dequeued: %s (priority=%d, wait_ms=%llu, "
                     "desc_len=%zu, timeout_ms=%u)",
                     rec->task_id, (int)rec->priority, (unsigned long long)wait_ms,
                     rec->task_description ? strlen(rec->task_description) : 0, rec->timeout_ms);
        airy_mtx_unlock(&svc->lock);

        char *selected = NULL;
        char *output = NULL;
        char *error = NULL;
        const uint64_t exec_t0 = sched_now_ms();

        task_info_t tinfo;
        __builtin_memset(&tinfo, 0, sizeof(tinfo));
        tinfo.task_id = rec->task_id;
        tinfo.task_description = rec->task_description;
        tinfo.priority = rec->priority;
        tinfo.timeout_ms = rec->timeout_ms;

        sched_result_t *sel = NULL;
        int sret = sched_service_schedule_task(svc, &tinfo, &sel);
        if (sret != AIRY_SUCCESS || !sel || !sel->selected_agent_id) {
            SVC_LOG_ERROR("sched: task %s select agent failed (rc=%d, sel=%p) — "
                          "check agent_d register_agent and agent availability",
                          rec->task_id, sret, (const void *)sel);
            error = AIRY_STRDUP("no available agent registered");
        } else {
            selected = AIRY_STRDUP(sel->selected_agent_id);
            if (!svc->executor) {
                sret = AIRY_ERR_SVC_NOT_READY;
                error = AIRY_STRDUP("task executor not injected");
                SVC_LOG_ERROR("sched: task %s executor not injected (set_executor "
                              "must be called before start_workers)",
                              rec->task_id);
            } else {
                SVC_LOG_INFO("sched: dispatching task %s to agent %s", rec->task_id,
                             sel->selected_agent_id);
                sret = svc->executor(sel->selected_agent_id, rec->task_description, &output);
                if (sret != AIRY_SUCCESS || !output) {
                    SVC_LOG_ERROR("sched: executor returned failure for task %s "
                                  "(rc=%d, output=%p)",
                                  rec->task_id, sret, (const void *)output);
                    error = AIRY_STRDUP("agent dispatch failed");
                }
            }
        }
        if (sel) {
            AIRY_FREE(sel->selected_agent_id);
            AIRY_FREE(sel);
        }

        const uint64_t exec_elapsed_ms = sched_now_ms() - exec_t0;

        airy_mtx_lock(&svc->lock);
        rec->selected_agent_id = selected;
        selected = NULL;
        rec->finished_at_ms = sched_now_ms();
        if (sret == AIRY_SUCCESS && output) {
            if (rec->timeout_ms > 0 && exec_elapsed_ms > rec->timeout_ms) {
                /* Execution returned success but exceeded the task-level
                 * timeout: report as timeout failure, closing the timeout_ms
                 * semantic loop (the synchronous executor cannot be
                 * interrupted mid-flight, at least the result layer applies). */
                rec->status = SCHED_TASK_STATUS_FAILED;
                rec->error = AIRY_STRDUP("task timed out");
                AIRY_FREE(output);
                output = NULL;
                SVC_LOG_WARN("Task timed out: %s (elapsed=%llu ms, limit=%u ms)", rec->task_id,
                             (unsigned long long)exec_elapsed_ms, rec->timeout_ms);
            } else {
                rec->status = SCHED_TASK_STATUS_COMPLETED;
                rec->output = output;
                output = NULL;
                SVC_LOG_INFO("Task completed: %s (agent=%s, output_len=%zu)", rec->task_id,
                             rec->selected_agent_id ? rec->selected_agent_id : "?",
                             rec->output ? strlen(rec->output) : 0);
            }
        } else {
            rec->status = SCHED_TASK_STATUS_FAILED;
            rec->error = error;
            error = NULL;
            SVC_LOG_ERROR("Task failed: %s (agent=%s, error=%s)", rec->task_id,
                          rec->selected_agent_id ? rec->selected_agent_id : "?",
                          rec->error ? rec->error : "unknown");
        }
        airy_mtx_unlock(&svc->lock);

        AIRY_FREE(selected);
        AIRY_FREE(output);
        AIRY_FREE(error);
    }
    return NULL;
}

int sched_service_submit_task(sched_service_t *service, const task_info_t *task_info,
                              char **out_task_id)
{
    if (!service || !task_info || !out_task_id || !service->initialized) {
        SVC_LOG_ERROR("sched_service_submit_task: NULL parameter or not initialized (service=%p, "
                      "task_info=%p, out_task_id=%p, initialized=%d)",
                      (const void *)service, (const void *)task_info, (const void *)out_task_id,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_task_id = NULL;

    /* task_id: use the client-provided one, else generate server-side
     * (timestamp+sequence, guaranteed unique). Generation must happen under
     * the lock — RPCs are processed concurrently through the thread pool
     * (thread_pool_max=8); task_seq++ outside the lock would race and produce
     * duplicate task_ids (data race). */
    char id_buf[64];
    const int use_generated = !(task_info->task_id && task_info->task_id[0]);

    airy_mtx_lock(&service->lock);
    if (use_generated) {
        snprintf(id_buf, sizeof(id_buf), "task_%llu_%zu", (unsigned long long)time(NULL),
                 service->task_seq++);
    } else {
        snprintf(id_buf, sizeof(id_buf), "%s", task_info->task_id);
    }
    for (size_t i = 0; i < service->task_count; i++) {
        if (strcmp(service->tasks[i]->task_id, id_buf) == 0) {
            airy_mtx_unlock(&service->lock);
            SVC_LOG_ERROR("sched_service_submit_task: duplicate task_id: %s", id_buf);
            return AIRY_ERR_ALREADY_EXISTS;
        }
    }
    size_t qlen =
        (service->queue_tail + AIRY_CAP_MAX_TASKS - service->queue_head) % AIRY_CAP_MAX_TASKS;
    if (qlen >= AIRY_CAP_MAX_TASKS - 1 || service->task_count >= AIRY_CAP_MAX_TASKS) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_ERROR("sched_service_submit_task: task queue full (task_count=%zu)",
                      service->task_count);
        return AIRY_ERR_OVERFLOW;
    }

    task_record_t *rec = (task_record_t *)AIRY_CALLOC(1, sizeof(task_record_t));
    if (!rec) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_ERROR("sched_service_submit_task: calloc failed for task record");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    rec->task_id = AIRY_STRDUP(id_buf);
    rec->task_description =
        AIRY_STRDUP(task_info->task_description ? task_info->task_description : "");
    if (!rec->task_id || !rec->task_description) {
        AIRY_FREE(rec->task_id);
        AIRY_FREE(rec->task_description);
        AIRY_FREE(rec);
        airy_mtx_unlock(&service->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    rec->priority = task_info->priority;
    rec->timeout_ms = task_info->timeout_ms;
    rec->status = SCHED_TASK_STATUS_PENDING;
    rec->created_at_ms = sched_now_ms();

    service->tasks[service->task_count++] = rec;
    service->queue[service->queue_tail] = rec;
    service->queue_tail = (service->queue_tail + 1) % AIRY_CAP_MAX_TASKS;

    airy_cond_signal(&service->queue_cond);
    airy_mtx_unlock(&service->lock);

    *out_task_id = AIRY_STRDUP(id_buf);
    SVC_LOG_INFO("Task queued: %s (priority=%d, timeout_ms=%u, queue_depth=%zu, "
                 "desc_len=%zu, worker=%s)",
                 id_buf, (int)task_info->priority, task_info->timeout_ms, qlen + 1,
                 rec->task_description ? strlen(rec->task_description) : 0,
                 service->worker_run ? "running" : "NOT STARTED (task will stay pending)");
    return AIRY_SUCCESS;
}

int sched_service_get_task(sched_service_t *service, const char *task_id, char **out_json)
{
    if (!service || !task_id || !out_json || !service->initialized) {
        SVC_LOG_ERROR("sched_service_get_task: NULL parameter or not initialized (service=%p, "
                      "task_id=%p, out_json=%p, initialized=%d)",
                      (const void *)service, (const void *)task_id, (const void *)out_json,
                      service ? service->initialized : -1);
        return AIRY_ERR_INVALID_PARAM;
    }
    *out_json = NULL;

    airy_mtx_lock(&service->lock);
    task_record_t *rec = NULL;
    for (size_t i = 0; i < service->task_count; i++) {
        if (strcmp(service->tasks[i]->task_id, task_id) == 0) {
            rec = service->tasks[i];
            break;
        }
    }
    if (!rec) {
        airy_mtx_unlock(&service->lock);
        SVC_LOG_WARN("sched_service_get_task: task not found (task_id=%s)", task_id);
        return AIRY_ERR_NOT_FOUND;
    }

    static const char *status_names[] = {"pending", "running", "completed", "failed", "canceled"};
    const char *sname =
        (rec->status < SCHED_TASK_STATUS_COUNT) ? status_names[rec->status] : "unknown";

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "task_id", rec->task_id);
        cJSON_AddStringToObject(root, "status", sname);
        cJSON_AddNumberToObject(root, "priority", (double)rec->priority);
        cJSON_AddNumberToObject(root, "timeout_ms", (double)rec->timeout_ms);
        cJSON_AddStringToObject(root, "selected_agent_id",
                                rec->selected_agent_id ? rec->selected_agent_id : "");
        cJSON_AddStringToObject(root, "output", rec->output ? rec->output : "");
        cJSON_AddStringToObject(root, "error", rec->error ? rec->error : "");
        cJSON_AddNumberToObject(root, "created_at_ms", (double)rec->created_at_ms);
        cJSON_AddNumberToObject(root, "finished_at_ms", (double)rec->finished_at_ms);
    }
    airy_mtx_unlock(&service->lock);

    if (!root)
        return AIRY_ERR_OUT_OF_MEMORY;
    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? AIRY_SUCCESS : AIRY_ERR_OUT_OF_MEMORY;
}

int sched_service_set_executor(sched_service_t *service, sched_task_executor_t executor)
{
    if (!service || !executor || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;

    service->executor = executor;
    return AIRY_SUCCESS;
}

int sched_service_start_workers(sched_service_t *service)
{
    if (!service || !service->initialized)
        return AIRY_ERR_INVALID_PARAM;
    if (service->worker_thread != AIRY_INVALID_THREAD)
        return AIRY_SUCCESS;

    service->worker_run = 1;
    if (airy_thread_create(&service->worker_thread, sched_worker_thread, service) != 0) {
        service->worker_run = 0;
        service->worker_thread = AIRY_INVALID_THREAD;
        SVC_LOG_ERROR("sched_service_start_workers: thread create failed");
        return AIRY_ERR_SVC_NOT_READY;
    }
    SVC_LOG_INFO("Scheduler worker thread started");

    service->dag_run = 1;
    if (airy_thread_create(&service->dag_thread, sched_dag_worker_thread, service) != 0) {
        service->dag_run = 0;
        service->dag_thread = AIRY_INVALID_THREAD;
        SVC_LOG_ERROR("sched_service_start_workers: dag thread create failed");
        return AIRY_ERR_SVC_NOT_READY;
    }
    SVC_LOG_INFO("Scheduler DAG worker thread started");
    return AIRY_SUCCESS;
}

void sched_service_stop_workers(sched_service_t *service)
{
    if (!service || !service->initialized)
        return;

    /* Stop the DAG thread first, then the task queue thread (coordinated via
     * the same dag_cond/lock). The dag thread may block on batch_cond (parallel
     * batch barrier) or dag_cond; both are broadcast. */
    if (service->dag_thread != AIRY_INVALID_THREAD) {
        airy_mtx_lock(&service->lock);
        service->dag_run = 0;
        airy_cond_broadcast(&service->dag_cond);
        airy_cond_broadcast(&service->batch_cond);
        airy_mtx_unlock(&service->lock);
        airy_thread_join(service->dag_thread, NULL);
        service->dag_thread = AIRY_INVALID_THREAD;
        SVC_LOG_INFO("Scheduler DAG worker thread stopped");
    }

    if (service->worker_thread == AIRY_INVALID_THREAD)
        return;

    airy_mtx_lock(&service->lock);
    service->worker_run = 0;

    airy_cond_broadcast(&service->queue_cond);
    airy_mtx_unlock(&service->lock);

    airy_thread_join(service->worker_thread, NULL);
    service->worker_thread = AIRY_INVALID_THREAD;
    SVC_LOG_INFO("Scheduler worker thread stopped");
}

int sched_service_cancel_task(sched_service_t *service, const char *task_id)
{
    if (!service || !task_id || !service->initialized) {
        SVC_LOG_ERROR("sched_service_cancel_task: NULL parameter or not initialized");
        return AIRY_ERR_INVALID_PARAM;
    }

    airy_mtx_lock(&service->lock);
    int found = 0;
    for (size_t i = 0; i < service->task_count; i++) {
        task_record_t *rec = service->tasks[i];
        if (strcmp(rec->task_id, task_id) != 0)
            continue;
        found = 1;
        if (rec->status != SCHED_TASK_STATUS_PENDING) {
            airy_mtx_unlock(&service->lock);
            SVC_LOG_WARN("sched_service_cancel_task: task %s not cancelable (status=%d)", task_id,
                         (int)rec->status);
            return AIRY_ERR_BUSY;
        }

        size_t qi = service->queue_head;
        size_t removed = 0;
        while (qi != service->queue_tail) {
            if (service->queue[qi] == rec) {
                service->queue[qi] = NULL;
                removed = 1;
                break;
            }
            qi = (qi + 1) % AIRY_CAP_MAX_TASKS;
        }
        if (removed) {

            size_t src = (qi + 1) % AIRY_CAP_MAX_TASKS;
            size_t dst = qi;
            while (src != service->queue_tail) {
                service->queue[dst] = service->queue[src];
                src = (src + 1) % AIRY_CAP_MAX_TASKS;
                dst = (dst + 1) % AIRY_CAP_MAX_TASKS;
            }
            service->queue_tail =
                (service->queue_tail + AIRY_CAP_MAX_TASKS - 1) % AIRY_CAP_MAX_TASKS;
        }
        rec->status = SCHED_TASK_STATUS_CANCELED;
        rec->finished_at_ms = sched_now_ms();
        rec->error = AIRY_STRDUP("canceled by user");
        SVC_LOG_INFO("Task canceled: %s (removed_from_queue=%d, pending_in_queue=%zu)", task_id,
                     (int)removed,
                     (service->queue_tail + AIRY_CAP_MAX_TASKS - service->queue_head) %
                         AIRY_CAP_MAX_TASKS);
        break;
    }
    airy_mtx_unlock(&service->lock);

    return found ? AIRY_SUCCESS : AIRY_ERR_NOT_FOUND;
}
