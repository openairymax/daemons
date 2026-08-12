/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service.h
 * @brief Agent service internal structure declarations.
 */

#ifndef AGENT_SERVICE_INTERNAL_H
#define AGENT_SERVICE_INTERNAL_H

#include "agent_service.h"

#include "platform.h"

#include <stddef.h>
#include <stdint.h>


/* Active invoke session cap (concurrent cancel lookup is a linear scan;
 * the cap prevents unbounded resources). Each session holds its own
 * cancel_token: handle_invoke registers, agent.cancel looks up and cancels. */
#define AGENT_INVOKE_SESSIONS_MAX 1024

typedef struct {
    char request_id[64];
    airy_cancel_token_t *token;
    int active;
} agent_invoke_session_t;


typedef struct {
    char *key;
    size_t index;
    int occupied;
} agent_hash_entry_t;

typedef struct {
    agent_hash_entry_t *entries;
    size_t capacity;
    size_t count;
} agent_hash_table_t;


/* Slot state machine:
 *   0 = free (reusable after spawn-failure rollback)
 *   1 = running (registered in hash table, child alive)
 *   3 = terminated (slot not reclaimed after terminate, keeps old semantics)
 *   4 = spawning (slot reserved, child starting, not yet in hash table) */
#define AGENT_STATUS_FREE 0
#define AGENT_STATUS_RUNNING 1
#define AGENT_STATUS_TERMINATED 3
#define AGENT_STATUS_SPAWNING 4

typedef struct {
    char *agent_id;
    char *spec;
    int status;
    uint64_t spawned_at;
    /* Fine-grained lock protecting this entry's status/child handles, so
     * fork / child IO never happens while holding the global lock (which
     * would serialize all agent operations). Each spawn/invoke/terminate
     * holds the global lock only briefly for index lookup; child lifecycle
     * operations run under this lock, allowing true parallelism for
     * thousands of agents. */
    airy_mtx_t entry_lock;
#if AIRY_PLATFORM_POSIX
    /* Stage5+ todo4: real spawn - handles after forking the agent runner
     * child (Python/Rust dual-language support).
     * child_pid > 0 means an active child; -1 means none (old logic fallback).
     * stdin_fd writes requests to the child, stdout_fd reads responses.
     * last_active: last successful child communication time (s), basis for
     * idle reaping. */
    pid_t child_pid;
    int stdin_fd;
    int stdout_fd;
    uint64_t last_active;
#endif
} agent_entry_internal_t;

struct agent_service {
    agent_entry_internal_t *agents;
    size_t agent_count;
    size_t max_agents;
    agent_hash_table_t agent_index;
    /* Global lock guarding only agent_count, the hash table and slot
     * allocation (fast path). Never do fork / network / child IO while
     * holding this lock. */
    airy_mtx_t lock;
    int initialized;

    /* ---- Perf monitor counters (10000-concurrency verified, atomic
     * lock-free updates) ----
     * At 10000 concurrency, per-request logging would flood IO, so only
     * atomic counting and duration aggregation (cumulative/max) are done,
     * sampled periodically by agent_d's perf monitor thread.
     * - Counts: spawn/invoke/terminate request totals and outcomes
     * - lock_wait_total: failed global-lock trylock probes (contention signal)
     * - Durations: aggregated in microseconds, 64-bit prevents overflow */
    airy_atomic_int_t m_spawn_total;
    airy_atomic_int_t m_spawn_ok;
    airy_atomic_int_t m_spawn_fail;
    airy_atomic_int_t m_invoke_total;
    airy_atomic_int_t m_invoke_ok;
    airy_atomic_int_t m_invoke_fail;
    airy_atomic_int_t m_terminate_total;
    airy_atomic_int_t m_lock_wait_total;
    airy_atomic_int_t m_peak_running;
    atomic_ullong m_spawn_us_total;
    atomic_ullong m_spawn_us_max;
    atomic_ullong m_invoke_us_total;
    atomic_ullong m_invoke_us_max;

    /* ---- Invoke session table (improvement 1 "cancel down-probe":
     * cross-process cancellation) ----
     * Guard lock is independent of svc->lock (invoke path holds entry_lock
     * during child IO; session register/unregister/cancel are short
     * critical sections; the separate lock avoids holding the global lock
     * for long periods). */
    airy_mtx_t session_lock;
    agent_invoke_session_t sessions[AGENT_INVOKE_SESSIONS_MAX];
};

#endif /* AGENT_SERVICE_INTERNAL_H */
