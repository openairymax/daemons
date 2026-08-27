// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file sched_daemon_internal.h
 * @brief Internal shared declarations of the sched_d daemon translation
 *        units (main.c / sched_rpc_handlers.c / sched_dispatch.c).
 * @details main.c was split by functional domain (single-responsibility):
 *          the JSON-RPC method handlers moved to sched_rpc_handlers.c (their
 *          on_*_method entry points were promoted from static because main.c
 *          registers them into the method dispatcher), the agent_d real
 *          dispatch chain moved to sched_dispatch.c (sched_dispatch_executor
 *          was promoted because main.c injects it as the service executor),
 *          and the daemon-wide service handle g_service is shared here.
 *          For use only by the sched_d daemon translation units.
 */

#ifndef AIRY_RT_SCHED_DAEMON_INTERNAL_H
#define AIRY_RT_SCHED_DAEMON_INTERNAL_H

#include "scheduler_service.h"
#include <cjson/cJSON.h>

/* Daemon-wide scheduler service handle (owned/lifecycled in main.c,
 * referenced by the JSON-RPC handlers). */
extern sched_service_t *g_service;

/* ---- JSON-RPC method entry points (defined in sched_rpc_handlers.c,
 *      registered by main.c into the method dispatcher) ---- */
void on_register_agent_method(cJSON *params, int id, void *user_data);
void on_unregister_agent_method(cJSON *params, int id, void *user_data);
void on_schedule_task_method(cJSON *params, int id, void *user_data);
void on_get_task_method(cJSON *params, int id, void *user_data);
void on_cancel_task_method(cJSON *params, int id, void *user_data);
void on_dag_submit_method(cJSON *params, int id, void *user_data);
void on_dag_status_method(cJSON *params, int id, void *user_data);
void on_dag_cancel_method(cJSON *params, int id, void *user_data);
void on_get_stats_method(cJSON *params, int id, void *user_data);
void on_health_check_method(cJSON *params, int id, void *user_data);
void on_checkpoint_save_method(cJSON *params, int id, void *user_data);

/* ---- Task-execution callback (defined in sched_dispatch.c, injected by
 *      main.c via sched_service_set_executor) ---- */
int sched_dispatch_executor(const char *agent_id, const char *task_description,
                            const char *workspace_dir, char **out_output);

#endif /* AIRY_RT_SCHED_DAEMON_INTERNAL_H */
