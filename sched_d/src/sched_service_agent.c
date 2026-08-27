// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_service_agent.c
 * @brief Scheduler service - agent registry and strategy-selection domain.
 * @details Implements the agent management public APIs in scheduler_service.h
 *          (register/unregister/update agent status, schedule_task strategy
 *          selection, stats/health/reload) plus the mac_framework agent sync
 *          helper. Moved out of sched_service_impl.c (single-responsibility
 *          split): the service create/destroy lifecycle lives in
 *          sched_service_impl.c, the task-queue/worker domain in
 *          sched_service_task.c.
 */

#include "scheduler_service.h"
#include "sched_service_internal.h"
#include "airy_memory.h"
#include "error.h"
#include "svc_logger.h"
#include "platform.h"
#include "multi_agent_collaboration.h"
#include <airymax/sched.h>

#include <stdio.h>
#include <string.h>

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
