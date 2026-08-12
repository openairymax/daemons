/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_service.h
 * @brief Public Agent service interface (agent.* namespace).
 *
 * Carries the runtime agent management logic of the former syscall_router.c
 * (airy_sys_agent_spawn/terminate/invoke/list), exposed as the service
 * core of the agent_d daemon.
 */

#ifndef AIRY_RT_AGENT_SERVICE_H
#define AIRY_RT_AGENT_SERVICE_H

#include <stddef.h>
#include <stdint.h>


#include "cancel_token.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_service agent_service_t;


agent_service_t *agent_service_create(size_t max_agents);
void agent_service_destroy(agent_service_t *svc);


/**
 * @brief Spawn a new agent.
 * @param spec Agent spec (JSON string)
 * @return AIRY_SUCCESS on success; *out_agent_id holds the new agent ID
 *         (caller frees with AIRY_FREE)
 */
int agent_service_spawn(agent_service_t *svc, const char *spec, char **out_agent_id);

/**
 * @brief Terminate the given agent.
 * @return AIRY_SUCCESS on success, AIRY_ERR_NOT_FOUND if not found
 */
int agent_service_terminate(agent_service_t *svc, const char *agent_id);

/**
 * @brief Invoke the given agent.
 * @param cancel_token [in] Cancel token, may be NULL: while invoke blocks
 *        reading the response it short-polls this token; on hit it ends the
 *        child gracefully (SIGTERM->2s->SIGKILL), closes with AbortedOutput
 *        and returns AIRY_ERR_CANCELED (distinct from the timeout path)
 * @return AIRY_SUCCESS on success; *out_output holds the JSON result string
 *         (caller frees with AIRY_FREE); AIRY_ERR_NOT_FOUND agent missing;
 *         AIRY_ERR_STATE_ERROR agent not running (terminated);
 *         AIRY_ERR_CANCELED execution cancelled (AbortedOutput)
 */
int agent_service_invoke(agent_service_t *svc, const char *agent_id, const char *input, size_t len,
                         airy_cancel_token_t *cancel_token, char **out_output);

/**
 * @brief List all agent IDs.
 * @return AIRY_SUCCESS on success; *out_agent_ids holds the ID array,
 *         *out_count the count (caller frees via agent_service_list_free)
 */
int agent_service_list(agent_service_t *svc, char ***out_agent_ids, size_t *out_count);


/**
 * @brief Register an invoke session (request_id -> cancel_token).
 *
 * Session basis for cross-process cancellation: the RPC layer's
 * handle_invoke registers a session before calling agent_service_invoke,
 * so callers can cancel by request_id via agent.cancel (RPC). On hit,
 * agent_service_invoke's select loop notices the token and terminates the
 * child (SIGTERM->SIGKILL), closing with AbortedOutput (distinct from
 * timeout -2).
 *
 * @param svc Service instance (non-NULL)
 * @param request_id Unique request ID (non-NULL, <= 63 chars)
 * @param out_token [out] Cancel token pointer of this session (BORROW, valid until unregistered)
 * @return AIRY_SUCCESS on registration; AIRY_ERR_INVALID_PARAM bad args;
 *         AIRY_ERR_BUSY session table full
 */
int agent_service_invoke_begin(agent_service_t *svc, const char *request_id,
                               airy_cancel_token_t **out_token);

/**
 * @brief Unregister an invoke session (call after invoke done/failed/cancelled).
 * @param svc Service instance
 * @param request_id Request ID used at registration
 */
void agent_service_invoke_end(agent_service_t *svc, const char *request_id);

/**
 * @brief Cancel an active invoke session by request_id.
 * @param svc Service instance
 * @param request_id Request ID
 * @return AIRY_SUCCESS session found and cancellation requested;
 *         AIRY_ERR_NOT_FOUND no matching active session
 */
int agent_service_invoke_cancel(agent_service_t *svc, const char *request_id);


size_t agent_service_count(agent_service_t *svc);
void agent_service_list_free(char **agent_ids, size_t count);


/**
 * @brief Service performance statistics snapshot (sampled periodically by
 *        the agent_d monitor thread under 10000-concurrency scenarios).
 *
 * All fields are cumulative since service creation; spawn/invoke durations
 * are aggregated in microseconds. Fields update via atomics, so reads need
 * no lock (loose consistency suffices for monitoring semantics).
 */
typedef struct {
    int spawn_total;
    int spawn_ok;
    int spawn_fail;
    int invoke_total;
    int invoke_ok;
    int invoke_fail;
    int terminate_total;
    int lock_wait_total;
    int peak_running;
    unsigned long long spawn_us_total;
    unsigned long long spawn_us_max;
    unsigned long long invoke_us_total;
    unsigned long long invoke_us_max;
} agent_perf_stats_t;

/**
 * @brief Get a service performance statistics snapshot.
 * @param svc Agent service instance
 * @param out Output statistics (non-NULL)
 * @return AIRY_SUCCESS on success
 */
int agent_service_get_perf(agent_service_t *svc, agent_perf_stats_t *out);

/**
 * @brief Reap idle agent child processes (P0-3: prevent child leaks).
 *
 * Iterates agents in running state with live children; when
 * now - last_active >= max_idle_s, terminates and reaps the child and
 * marks the slot terminated (array is not compacted).
 *
 * @param svc Agent service instance
 * @param max_idle_s Idle threshold in seconds, 0 = reap immediately
 * @return AIRY_SUCCESS on success
 */
int agent_service_reap_idle(agent_service_t *svc, uint64_t max_idle_s);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_AGENT_SERVICE_H */
