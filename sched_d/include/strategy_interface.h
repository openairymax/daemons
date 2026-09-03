/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file strategy_interface.h
 * @brief Scheduling-strategy interface definitions.
 * @details Defines the interface every scheduling strategy must implement.
 */

#ifndef AIRY_RT_STRATEGY_INTERFACE_H
#define AIRY_RT_STRATEGY_INTERFACE_H

#include "scheduler_service.h"

/** @brief Scheduling-strategy interface. */
typedef struct {
    /**
     * @brief Create a strategy.
     * @param manager Config info
     * @param data Output parameter, returns the strategy data
     * @return 0 on success, non-zero error code
     */
    int (*create)(const sched_config_t *manager, void **data);

    /**
     * @brief Destroy a strategy.
     * @param data Strategy data
     * @return 0 on success, non-zero error code
     */
    int (*destroy)(void *data);

    /**
     * @brief Register an agent.
     * @param data Strategy data
     * @param agent_info Agent info
     * @return 0 on success, non-zero error code
     */
    int (*register_agent)(void *data, const agent_info_t *agent_info);

    /**
     * @brief Unregister an agent.
     * @param data Strategy data
     * @param agent_id Agent ID
     * @return 0 on success, non-zero error code
     */
    int (*unregister_agent)(void *data, const char *agent_id);

    /**
     * @brief Update an agent's status.
     * @param data Strategy data
     * @param agent_info Agent info
     * @return 0 on success, non-zero error code
     */
    int (*update_agent_status)(void *data, const agent_info_t *agent_info);

    /**
     * @brief Run a scheduling decision.
     * @param data Strategy data
     * @param task_info Task info
     * @param result Output parameter, returns the scheduling result
     * @return 0 on success, non-zero error code
     */
    int (*schedule)(void *data, const sched_task_info_t *task_info, sched_result_t **result);

    /**
     * @brief Get the strategy name.
     * @return Strategy name
     */
    const char *(*get_name)();

    /**
     * @brief Get the number of available agents.
     * @param data Strategy data
     * @return Available agent count
     */
    size_t (*get_available_agent_count)(void *data);

    /**
     * @brief Get the total agent count.
     * @param data Strategy data
     * @return Total agent count
     */
    size_t (*get_total_agent_count)(void *data);
} strategy_interface_t;

/**
 * @brief Get the round-robin scheduling-strategy interface.
 * @return Round-robin scheduling-strategy interface
 */
const strategy_interface_t *get_round_robin_strategy();

/**
 * @brief Get the weighted scheduling-strategy interface.
 * @return Weighted scheduling-strategy interface
 */
const strategy_interface_t *get_weighted_strategy();

/**
 * @brief Get the machine-learning-based scheduling-strategy interface.
 * @return ML-based scheduling-strategy interface
 */
const strategy_interface_t *get_ml_based_strategy();

/**
 * @brief Get the priority-based scheduling-strategy interface.
 * @return Priority-based scheduling-strategy interface
 */
const strategy_interface_t *get_priority_based_strategy();

#endif /* AIRY_RT_STRATEGY_INTERFACE_H */
