/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file agent_service_internal.h
 * @brief Internal declarations shared across agent service files.
 */

#ifndef AIRY_RT_DAEMON_AGENT_D_AGENT_SERVICE_INTERNAL_H
#define AIRY_RT_DAEMON_AGENT_D_AGENT_SERVICE_INTERNAL_H

#include "service.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_ID_LEN 33
#define AGENT_RESP_BUF_SIZE 65536

/* Hash table and ID generation (service.c) */
int agent_ht_insert(agent_hash_table_t *ht, const char *key, size_t index);
ssize_t agent_ht_lookup(agent_hash_table_t *ht, const char *key);
void agent_generate_agent_id(char *buf, size_t buf_size);

/* Perf/lock helpers (service.c) */
uint64_t agent_perf_now_us(void);
void agent_lock_svc(agent_service_t *svc);
void agent_perf_accumulate(atomic_ullong *us_total, atomic_ullong *us_max, uint64_t elapsed_us);

#if AIRY_PLATFORM_POSIX
/* Child process communication (service_child.c) */
int agent_invoke_timeout_s(void);
int agent_spawn_ready_timeout_s(void);
int agent_write_all(int fd, const char *buf, size_t len);
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
