/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file a2a_service.h
 * @brief Public A2A service interface (a2a.* namespace).
 *
 * Wraps the a2a_v03_adapter library as the service core of the a2a_d
 * daemon: agent registration/discovery, task lifecycle, and messaging.
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
 * @param max_agents Max agents (0 = default 256)
 * @param max_tasks Max tasks (0 = default 4096)
 * @return Service instance, or NULL on failure
 */
a2a_service_t *a2a_service_create(size_t max_agents, size_t max_tasks);
void a2a_service_destroy(a2a_service_t *svc);


/**
 * @brief Register an agent.
 * @param card_json Agent Card JSON string; fields: id, name, description,
 *        url, version, protocol_version (int, default 3), capabilities (int),
 *        available (bool, default true), skills (array, optional)
 * @return AIRY_SUCCESS on success
 */
int a2a_service_register_agent(a2a_service_t *svc, const char *card_json);

/**
 * @brief Unregister an agent.
 * @return AIRY_SUCCESS on success, AIRY_ERR_NOT_FOUND if not found
 */
int a2a_service_unregister_agent(a2a_service_t *svc, const char *agent_id);

/**
 * @brief Get an agent card.
 * @return AIRY_SUCCESS on success; *out_card_json holds the card JSON string
 *         (caller frees via a2a_service_card_free); AIRY_ERR_NOT_FOUND if not found
 */
int a2a_service_get_agent_card(a2a_service_t *svc, const char *agent_id, char **out_card_json);

/**
 * @brief Discover agents.
 * @param capability Capability filter string, may be NULL (no filter)
 * @param skill_name Skill filter string, may be NULL (no filter)
 * @return AIRY_SUCCESS on success; *out_results_json holds the card JSON array
 *         (caller frees via a2a_service_results_free), *out_count the hit count
 */
int a2a_service_discover_agents(a2a_service_t *svc, const char *capability, const char *skill_name,
                                char **out_results_json, size_t *out_count);


/**
 * @brief Create a task.
 * @return AIRY_SUCCESS on success; *out_task_json holds the task JSON string
 *         (caller frees via a2a_service_task_free)
 */
int a2a_service_create_task(a2a_service_t *svc, const char *agent_id, const char *description,
                            const char *input_json, char **out_task_json);

/**
 * @brief Update task state.
 * @param state New state value (a2a_task_state_t enum)
 * @param output_json Output JSON, may be NULL
 * @param progress Progress in [0.0, 1.0]
 * @return AIRY_SUCCESS on success, AIRY_ERR_NOT_FOUND if not found
 */
int a2a_service_update_task(a2a_service_t *svc, const char *task_id, int state,
                            const char *output_json, double progress);

/**
 * @brief Cancel a task.
 * @param reason Cancellation reason, may be NULL
 * @return AIRY_SUCCESS on success, AIRY_ERR_NOT_FOUND if not found
 */
int a2a_service_cancel_task(a2a_service_t *svc, const char *task_id, const char *reason);

/**
 * @brief Get a task.
 * @return AIRY_SUCCESS on success; *out_task_json holds the task JSON string
 *         (caller frees via a2a_service_task_free); AIRY_ERR_NOT_FOUND if not found
 */
int a2a_service_get_task(a2a_service_t *svc, const char *task_id, char **out_task_json);


/**
 * @brief Send a message to a target agent.
 * @param role Message role (e.g. "user")
 * @param content_json Message content JSON string
 * @return AIRY_SUCCESS on success; *out_response_json holds the response JSON
 *         array (caller frees via a2a_service_results_free), *out_response_count
 *         the number of responses
 */
int a2a_service_send_message(a2a_service_t *svc, const char *target_agent_id, const char *role,
                             const char *content_json, char **out_response_json,
                             size_t *out_response_count);


size_t a2a_service_count(a2a_service_t *svc);
size_t a2a_service_task_count(a2a_service_t *svc);

void a2a_service_card_free(char *card_json);
void a2a_service_task_free(char *task_json);
void a2a_service_results_free(char *results_json);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_A2A_SERVICE_H */
