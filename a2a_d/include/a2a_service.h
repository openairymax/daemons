/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file a2a_service.h
 * @brief Public A2A service interface (a2a.* namespace).
 *
 * 服务核经 proto_registry 注入式消费 A2A 适配器：本模块只持有
 * protocol_adapter_t 句柄与注入上下文，不直接依赖任何厂商库。
 * 机制（daemon）与策略（适配器）通过统一 ops 函数表解耦。
 */

#ifndef AIRY_RT_A2A_SERVICE_H
#define AIRY_RT_A2A_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct a2a_service a2a_service_t;


/**
 * @brief Create an A2A service instance.
 *
 * Resolves the injected A2A adapter through the process-wide protocol
 * registry and initialises its context.
 *
 * @return Service instance, or NULL on failure
 */
a2a_service_t *a2a_service_create(void);
void a2a_service_destroy(a2a_service_t *svc);


/**
 * @brief Register an agent.
 * @param card_json Agent Card JSON string; fields: id, name, description,
 *        url, version, protocol_version (int, default 3), capabilities (int),
 *        available (bool, default true), skills (array, optional)
 * @param out_result_json On success holds the adapter result JSON
 *        (caller frees via a2a_result_free)
 * @return AIRY_SUCCESS on success
 */
int a2a_service_register_agent(a2a_service_t *svc, const char *card_json, char **out_result_json);

/**
 * @brief Unregister an agent.
 * @return AIRY_SUCCESS on success, AIRY_ERR_NOT_FOUND if not found
 */
int a2a_service_unregister_agent(a2a_service_t *svc, const char *agent_id,
                                 char **out_result_json);

/**
 * @brief Get an agent card.
 * @return AIRY_SUCCESS on success; *out_result_json holds the card JSON string
 *         (caller frees via a2a_result_free); AIRY_ERR_NOT_FOUND if not found
 */
int a2a_service_get_agent_card(a2a_service_t *svc, const char *agent_id,
                               char **out_result_json);

/**
 * @brief Discover agents.
 * @param capability Capability filter string, may be NULL (no filter)
 * @param skill_name Skill filter string, may be NULL (no filter)
 * @return AIRY_SUCCESS on success; *out_result_json holds
 *         {"agents":[...],"count":N} (caller frees via a2a_result_free)
 */
int a2a_service_discover_agents(a2a_service_t *svc, const char *capability,
                                const char *skill_name, char **out_result_json);


/**
 * @brief Create a task.
 * @return AIRY_SUCCESS on success; *out_result_json holds {"task":{...}}
 *         (caller frees via a2a_result_free)
 */
int a2a_service_create_task(a2a_service_t *svc, const char *agent_id, const char *description,
                            const char *input_json, char **out_result_json);

/**
 * @brief Update task state.
 * @param state New state value (a2a_task_state_t enum)
 * @param output_json Output JSON, may be NULL
 * @param progress Progress in [0.0, 1.0]
 * @return AIRY_SUCCESS on success, AIRY_ERR_NOT_FOUND if not found
 */
int a2a_service_update_task(a2a_service_t *svc, const char *task_id, int state,
                            const char *output_json, double progress, char **out_result_json);

/**
 * @brief Cancel a task.
 * @param reason Cancellation reason, may be NULL
 * @return AIRY_SUCCESS on success, AIRY_ERR_NOT_FOUND if not found
 */
int a2a_service_cancel_task(a2a_service_t *svc, const char *task_id, const char *reason,
                            char **out_result_json);

/**
 * @brief Get a task.
 * @return AIRY_SUCCESS on success; *out_result_json holds {"task":{...}}
 *         (caller frees via a2a_result_free); AIRY_ERR_NOT_FOUND if not found
 */
int a2a_service_get_task(a2a_service_t *svc, const char *task_id, char **out_result_json);


/**
 * @brief Send a message to a target agent.
 * @param role Message role (e.g. "user")
 * @param content_json Message content JSON string
 * @return AIRY_SUCCESS on success; *out_result_json holds
 *         {"responses":[...],"count":N} (caller frees via a2a_result_free)
 */
int a2a_service_send_message(a2a_service_t *svc, const char *target_agent_id, const char *role,
                             const char *content_json, char **out_result_json);


/**
 * @brief Read current agent/task counters from the adapter.
 * @param out_agents Optional, receives the agent count
 * @param out_tasks Optional, receives the task count
 * @return AIRY_SUCCESS on success
 */
int a2a_service_stats(a2a_service_t *svc, size_t *out_agents, size_t *out_tasks);

void a2a_result_free(char *result_json);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_A2A_SERVICE_H */
