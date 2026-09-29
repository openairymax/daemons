/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file sched_daemon_internal.h
 * @brief Internal shared declarations of the sched_d daemon translation
 *        units (svc.c / sched_rpc_handlers.c / sched_dispatch.c).
 * @details RPC method entry points (svc_on_*_sched_d) and the daemon
 *          lifecycle hooks are declared in the generated svc_sched_d.h
 *          (L3 SSoT); this header only carries the daemon-wide service
 *          handle and the agent_d dispatch executor. For use only by the
 *          sched_d daemon translation units.
 */

#ifndef AIRY_RT_SCHED_DAEMON_INTERNAL_H
#define AIRY_RT_SCHED_DAEMON_INTERNAL_H

#include "scheduler_service.h"

/* Daemon-wide scheduler service handle (owned/lifecycled in svc.c,
 * referenced by the JSON-RPC handlers). */
extern sched_service_t *g_service;

/* Task-execution callback (defined in sched_dispatch.c, injected by
 * svc.c via sched_service_set_executor) */
int sched_dispatch_executor(const char *agent_id, const char *task_description,
                            const char *workspace_dir, char **out_output);

#endif /* AIRY_RT_SCHED_DAEMON_INTERNAL_H */
