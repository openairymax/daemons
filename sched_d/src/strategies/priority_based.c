// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/**
 * @file priority_based.c
 * @brief 基于优先级的调度策略实现
 * @details 根据任务优先级和Agent权重进行智能调度
 */

#include "scheduler_service.h"
#include "strategy_interface.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 基于优先级的调度策略数据
 */
typedef struct {
    agent_info_t **agents;
    size_t agent_count;
    size_t max_agents;
    float *priority_weights;
    size_t priority_levels;
} priority_based_data_t;

/**
 * @brief 创建基于优先级的调度策略
 * @param manager 配置信息
 * @param data 输出参数，返回策略数据
 * @return 0 表示成功，非 0 表示错误码
 */
static int priority_based_create(const sched_config_t *manager, void **data)
{
    priority_based_data_t *pbd =
        (priority_based_data_t *)AIRY_MALLOC(sizeof(priority_based_data_t));
    if (!pbd) {
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    pbd->max_agents = manager->max_agents;

    if (pbd->max_agents > SIZE_MAX / sizeof(agent_info_t *)) {
        AIRY_FREE(pbd);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    pbd->agents = (agent_info_t **)AIRY_MALLOC(sizeof(agent_info_t *) * pbd->max_agents);
    if (!pbd->agents) {
        AIRY_FREE(pbd);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    pbd->agent_count = 0;

    pbd->priority_levels = 10;
    SAFE_MALLOC_ARRAY(pbd->priority_weights, pbd->priority_levels, sizeof(float));
    if (pbd->priority_weights) {
        for (size_t i = 0; i < pbd->priority_levels; i++) {
            pbd->priority_weights[i] = expf((float)i / 2.0f);
        }
    }

    *data = pbd;
    return 0;
}

/**
 * @brief Destroy the priority-based scheduling strategy
 * @param data Strategy data
 * @return 0 on success, non-zero error code
 */
static int priority_based_destroy(void *data)
{
    if (!data) {
        return 0;
    }

    priority_based_data_t *pbd = (priority_based_data_t *)data;

    if (pbd->agents) {
        for (size_t i = 0; i < pbd->agent_count; i++) {
            if (pbd->agents[i]) {
                AIRY_FREE(pbd->agents[i]->agent_id);
                AIRY_FREE(pbd->agents[i]->agent_name);
                AIRY_FREE(pbd->agents[i]);
            }
        }
        AIRY_FREE(pbd->agents);
    }

    if (pbd->priority_weights) {
        AIRY_FREE(pbd->priority_weights);
    }

    AIRY_FREE(pbd);
    return 0;
}

/**
 * @brief Register an agent
 * @param data Strategy data
 * @param agent_info Agent info
 * @return 0 on success, non-zero error code
 */
static int priority_based_register_agent(void *data, const agent_info_t *agent_info)
{
    if (!data || !agent_info) {
        return AIRY_ERR_INVALID_PARAM;
    }

    priority_based_data_t *pbd = (priority_based_data_t *)data;

    if (pbd->agent_count >= pbd->max_agents) {
        return AIRY_ERR_OVERFLOW;
    }

    for (size_t i = 0; i < pbd->agent_count; i++) {
        if (strcmp(pbd->agents[i]->agent_id, agent_info->agent_id) == 0) {
            AIRY_FREE(pbd->agents[i]->agent_id);
            AIRY_FREE(pbd->agents[i]->agent_name);

            pbd->agents[i]->agent_id = AIRY_STRDUP(agent_info->agent_id);
            pbd->agents[i]->agent_name = AIRY_STRDUP(agent_info->agent_name);
            if (!pbd->agents[i]->agent_id || !pbd->agents[i]->agent_name) {
                AIRY_FREE(pbd->agents[i]->agent_id);
                AIRY_FREE(pbd->agents[i]->agent_name);
                pbd->agents[i]->agent_id = NULL;
                pbd->agents[i]->agent_name = NULL;
                return AIRY_ERR_OUT_OF_MEMORY;
            }
            pbd->agents[i]->load_factor = agent_info->load_factor;
            pbd->agents[i]->success_rate = agent_info->success_rate;
            pbd->agents[i]->avg_response_time_ms = agent_info->avg_response_time_ms;
            pbd->agents[i]->is_available = agent_info->is_available;
            pbd->agents[i]->weight = agent_info->weight;

            return 0;
        }
    }

    agent_info_t *new_agent = (agent_info_t *)AIRY_MALLOC(sizeof(agent_info_t));
    if (!new_agent) {
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    new_agent->agent_id = AIRY_STRDUP(agent_info->agent_id);
    new_agent->agent_name = AIRY_STRDUP(agent_info->agent_name);
    if (!new_agent->agent_id || !new_agent->agent_name) {
        AIRY_FREE(new_agent->agent_id);
        AIRY_FREE(new_agent->agent_name);
        AIRY_FREE(new_agent);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    new_agent->load_factor = agent_info->load_factor;
    new_agent->success_rate = agent_info->success_rate;
    new_agent->avg_response_time_ms = agent_info->avg_response_time_ms;
    new_agent->is_available = agent_info->is_available;
    new_agent->weight = agent_info->weight;

    pbd->agents[pbd->agent_count++] = new_agent;
    return 0;
}

/**
 * @brief Unregister an agent
 * @param data Strategy data
 * @param agent_id Agent ID
 * @return 0 on success, non-zero error code
 */
static int priority_based_unregister_agent(void *data, const char *agent_id)
{
    if (!data || !agent_id) {
        return AIRY_ERR_INVALID_PARAM;
    }

    priority_based_data_t *pbd = (priority_based_data_t *)data;

    for (size_t i = 0; i < pbd->agent_count; i++) {
        if (strcmp(pbd->agents[i]->agent_id, agent_id) == 0) {
            AIRY_FREE(pbd->agents[i]->agent_id);
            AIRY_FREE(pbd->agents[i]->agent_name);
            AIRY_FREE(pbd->agents[i]);

            for (size_t j = i; j < pbd->agent_count - 1; j++) {
                pbd->agents[j] = pbd->agents[j + 1];
            }

            pbd->agent_count--;
            return 0;
        }
    }

    return AIRY_ERR_NOT_FOUND;
}

/**
 * @brief Update agent status
 * @param data Strategy data
 * @param agent_info Agent info
 * @return 0 on success, non-zero error code
 */
static int priority_based_update_agent_status(void *data, const agent_info_t *agent_info)
{
    return priority_based_register_agent(data, agent_info);
}

/**
 * @brief Calculate the task-agent match score
 * @param agent Agent info
 * @param task Task info
 * @param priority_weight Priority weight
 * @return Match score
 */
static float calculate_match_score(const agent_info_t *agent, const task_info_t *task,
                                   float priority_weight)
{
    if (!agent->is_available || agent->load_factor >= 0.9) {
        return AIRY_EINVAL;
    }

    float score = 0.0f;

    score += agent->success_rate * 0.4f;

    score += (1.0f - agent->load_factor) * 0.3f;

    float response_factor = 1.0f / (1.0f + agent->avg_response_time_ms / 1000.0f);
    score += response_factor * 0.2f;

    score += agent->weight * 0.1f;

    score *= priority_weight;

    return score;
}

/**
 * @brief Run priority-based scheduling
 * @param data Strategy data
 * @param task_info Task info
 * @param result Output param, returns the scheduling result
 * @return 0 on success, non-zero error code
 */
static int priority_based_schedule(void *data, const task_info_t *task_info,
                                   sched_result_t **result)
{
    if (!data || !task_info || !result) {
        return AIRY_ERR_INVALID_PARAM;
    }

    priority_based_data_t *pbd = (priority_based_data_t *)data;

    if (pbd->agent_count == 0) {
        return AIRY_ERR_NOT_FOUND;
    }

    int task_priority = 5;
    if (task_info->priority >= 0) {
        task_priority = task_info->priority;
    }

    // Clamp the priority to the valid range
    if (task_priority < 0)
        task_priority = 0;
    if (task_priority >= (int)pbd->priority_levels)
        task_priority = pbd->priority_levels - 1;

    float priority_weight = 1.0f;
    if (pbd->priority_weights && task_priority < (int)pbd->priority_levels) {
        priority_weight = pbd->priority_weights[task_priority];
    }

    agent_info_t *best_agent = NULL;
    float best_score = -1.0f;

    for (size_t i = 0; i < pbd->agent_count; i++) {
        agent_info_t *agent = pbd->agents[i];
        float score = calculate_match_score(agent, task_info, priority_weight);

        if (score > best_score) {
            best_score = score;
            best_agent = agent;
        }
    }

    if (!best_agent) {
        return AIRY_ERR_NOT_FOUND;
    }

    sched_result_t *res = (sched_result_t *)AIRY_MALLOC(sizeof(sched_result_t));
    if (!res) {
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    res->selected_agent_id = AIRY_STRDUP(best_agent->agent_id);
    res->confidence = best_score;
    res->estimated_time_ms = best_agent->avg_response_time_ms;

    *result = res;
    return 0;
}

/**
 * @brief Get the priority-based scheduling strategy name
 * @return Strategy name
 */
static const char *priority_based_get_name()
{
    return "priority_based";
}

/**
 * @brief Get the number of available agents
 * @param data Strategy data
 * @return Number of available agents
 */
static size_t priority_based_get_available_agent_count(void *data)
{
    if (!data) {
        return 0;
    }

    priority_based_data_t *pbd = (priority_based_data_t *)data;
    size_t count = 0;

    for (size_t i = 0; i < pbd->agent_count; i++) {
        if (pbd->agents[i]->is_available) {
            count++;
        }
    }

    return count;
}

/**
 * @brief Get the total number of agents
 * @param data Strategy data
 * @return Total number of agents
 */
static size_t priority_based_get_total_agent_count(void *data)
{
    if (!data) {
        return 0;
    }

    priority_based_data_t *pbd = (priority_based_data_t *)data;
    return pbd->agent_count;
}

/**
 * @brief Priority-based scheduling strategy interface
 */
static const strategy_interface_t priority_based_strategy = {
    .create = priority_based_create,
    .destroy = priority_based_destroy,
    .register_agent = priority_based_register_agent,
    .unregister_agent = priority_based_unregister_agent,
    .update_agent_status = priority_based_update_agent_status,
    .schedule = priority_based_schedule,
    .get_name = priority_based_get_name,
    .get_available_agent_count = priority_based_get_available_agent_count,
    .get_total_agent_count = priority_based_get_total_agent_count};

/**
 * @brief Get the priority-based scheduling strategy interface
 * @return Priority-based scheduling strategy interface
 */
const strategy_interface_t *get_priority_based_strategy()
{
    return &priority_based_strategy;
}