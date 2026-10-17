/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_service_internal.h
 * @brief Internal declarations shared across agent service files.
 */

#ifndef AIRY_RT_DAEMON_AGENT_D_AGENT_SERVICE_INTERNAL_H
#define AIRY_RT_DAEMON_AGENT_D_AGENT_SERVICE_INTERNAL_H

#include "service.h"

#include "airy_memory.h"
#include "airy_types.h"
#include "error.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if AIRY_PLATFORM_POSIX
#include <sys/select.h>
#include <sys/wait.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_RESP_BUF_SIZE 65536

/* Perf/lock helpers (service.c) */
uint64_t agent_perf_now_us(void);
void agent_lock_svc(agent_service_t *svc);
void agent_perf_accumulate(atomic_ullong *us_total, atomic_ullong *us_max, uint64_t elapsed_us);

/* Invoke/spawn budget parsers (service_child.c): platform-neutral so the
 * platform-neutral invoke entry points can link them on every platform. */
int agent_invoke_timeout_s(void);
int agent_spawn_ready_timeout_s(void);

#if AIRY_PLATFORM_POSIX
/* Child process communication (service_child.c) */
int agent_read_line_timeout(int fd, char *buf, size_t buf_size, int timeout_s);
int agent_read_line_timeout_ex(int fd, char *buf, size_t buf_size, int timeout_s,
                               airy_cancel_token_t *token);
int agent_spawn_child(const char *spec, const char *agent_id, pid_t *out_pid, int *out_stdin,
                      int *out_stdout);
void agent_kill_and_reap(pid_t *pid_ptr, int *stdin_ptr, int *stdout_ptr);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_D_AGENT_SERVICE_INTERNAL_H */
