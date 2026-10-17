// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file service.c
 * @brief Agent service implementation: spawn/terminate/invoke/list.
 *
 * Extracted from g_runtime.agents[] logic in gateway/src/utils/syscall/syscall_router.c
 * and refactored into a standalone, self-contained service module. The
 * agent_d daemon holds an agent_service_t instance and exposes the agent.*
 * namespace over a Unix socket.
 *
 * Design notes:
 * - Index is the shared commons hindex (djb2 + tombstones), §130 SSoT
 * - Thread safety: all public interfaces take the lock
 * - Agent ID: commons airy_oid atom (32-char hex, no external deps)
 * - Terminate does not reclaim slots: only sets status=3, no compaction
 */

#include "agent_service_internal.h"
#include "platform.h"

/* Default max concurrent agents: supports thousands in parallel (design
 * intent). Overridable via AIRY_MAX_AGENTS env var or daemon config
 * max_agents (capped at 65535). */
#define AGENT_DEFAULT_MAX_AGENTS 10000

#define AGENT_HASH_LOAD_FACTOR 4 /* capacity = max_agents * 4 */

/* Monotonic microsecond clock for spawn/invoke latency aggregation and
 * slow-request detection. */
uint64_t agent_perf_now_us(void)
{
    return airy_time_ns() / 1000;
}

/* Global lock acquisition: trylock first; on failure count one lock
 * contention (atomic) then block on the lock. Under 10000-way concurrency,
 * lock contention is the primary bottleneck signal; probing with trylock
 * quantifies wait counts without modifying airy_mtx. On return the caller
 * holds the global lock. */
void agent_lock_svc(agent_service_t *svc)
{
    if (airy_mtx_trylock(&svc->lock) != 0) {
        airy_atomic_fetch_add(&svc->m_lock_wait_total, 1);
        airy_mtx_lock(&svc->lock);
    }
}

void agent_perf_accumulate(atomic_ullong *us_total, atomic_ullong *us_max,
                           uint64_t elapsed_us)
{
    atomic_fetch_add_explicit(us_total, elapsed_us, memory_order_relaxed);
    unsigned long long cur = atomic_load_explicit(us_max, memory_order_relaxed);
    while (elapsed_us > cur) {
        if (atomic_compare_exchange_weak_explicit(us_max, &cur, elapsed_us, memory_order_relaxed,
                                                  memory_order_relaxed))
            break;
    }
}

agent_service_t *agent_service_create(size_t max_agents)
{
    if (max_agents == 0)
        max_agents = AGENT_DEFAULT_MAX_AGENTS;

    agent_service_t *svc = (agent_service_t *)AIRY_CALLOC(1, sizeof(agent_service_t));
    if (!svc)
        return NULL;

    svc->max_agents = max_agents;
    svc->agents = (agent_entry_internal_t *)AIRY_CALLOC(max_agents, sizeof(agent_entry_internal_t));
    if (!svc->agents) {
        AIRY_FREE(svc);
        return NULL;
    }

    if (hindex_init(&svc->agent_index, max_agents * AGENT_HASH_LOAD_FACTOR) != AIRY_SUCCESS) {
        AIRY_FREE(svc->agents);
        AIRY_FREE(svc);
        return NULL;
    }

    airy_mtx_init(&svc->lock);
    airy_mtx_init(&svc->session_lock);
    svc->agent_count = 0;
    svc->initialized = 1;
    /* Initialize the per-slot fine-grained lock (concurrency refactor:
     * child-process lifecycle operations run under entry_lock without taking
     * the global lock) */
    for (size_t i = 0; i < max_agents; i++) {
        airy_mtx_init(&svc->agents[i].entry_lock);
        svc->agents[i].status = AGENT_STATUS_FREE;
    }
#if AIRY_PLATFORM_POSIX
    /* After forking a child, child exit causes the pipe write end to receive
     * SIGPIPE. Ignore the signal so write returns EPIPE, handled by the caller. */
    signal(SIGPIPE, SIG_IGN);
#endif
    SVC_LOG_INFO("Agent service created (max_agents=%zu)", max_agents);
    return svc;
}

void agent_service_destroy(agent_service_t *svc)
{
    if (!svc)
        return;

    airy_mtx_lock(&svc->lock);
    for (size_t i = 0; i < svc->agent_count; i++) {
        agent_entry_internal_t *agent = &svc->agents[i];
#if AIRY_PLATFORM_POSIX
        if (agent->child_pid > 0) {
            agent_kill_and_reap(&agent->child_pid, &agent->stdin_fd, &agent->stdout_fd);
        }
#endif
        AIRY_FREE(agent->agent_id);
        AIRY_FREE(agent->spec);
        agent->agent_id = NULL;
        agent->spec = NULL;
    }
    AIRY_FREE(svc->agents);
    hindex_free(&svc->agent_index);
    svc->agent_count = 0;
    svc->max_agents = 0;
    svc->initialized = 0;
    airy_mtx_unlock(&svc->lock);
    airy_mtx_destroy(&svc->lock);
    airy_mtx_destroy(&svc->session_lock);
    AIRY_FREE(svc);
}
