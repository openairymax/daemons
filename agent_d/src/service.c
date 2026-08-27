// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
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
 * - Own hash table (djb2, same origin as syscall_router.c but decoupled)
 * - Thread safety: all public interfaces take the lock
 * - Agent ID: 32-char hex (timestamp + counter, no external deps)
 * - Terminate does not reclaim slots: only sets status=3, no compaction
 */

#include "service.h"

#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if AIRY_PLATFORM_POSIX
/* Stage5+ todo4: POSIX primitives needed for real spawn. platform.h already
 * brings in unistd.h / signal.h / errno.h / fcntl.h / sys/types.h; add the
 * headers for waitpid and select here. */
#include <sys/select.h>
#include <sys/wait.h>
#endif

#include "agent_service_internal.h"

/* Default max concurrent agents: supports thousands in parallel (design
 * intent). Overridable via AIRY_MAX_AGENTS env var or daemon config
 * max_agents (capped at 65535). */
#define AGENT_DEFAULT_MAX_AGENTS 10000

#define AGENT_HASH_LOAD_FACTOR 4 /* capacity = max_agents * 4 */

static unsigned long agent_hash_fn(const char *str)
{

    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

static int agent_ht_init(agent_hash_table_t *ht, size_t capacity)
{
    if (!ht || capacity == 0)
        return AIRY_ERR_INVALID_PARAM;

    ht->entries = (agent_hash_entry_t *)AIRY_CALLOC(capacity, sizeof(agent_hash_entry_t));
    if (!ht->entries) {
        ht->capacity = 0;
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    ht->capacity = capacity;
    ht->count = 0;
    return AIRY_SUCCESS;
}

static void agent_ht_destroy(agent_hash_table_t *ht)
{
    if (!ht || !ht->entries)
        return;
    for (size_t i = 0; i < ht->capacity; i++) {
        AIRY_FREE(ht->entries[i].key);
    }
    AIRY_FREE(ht->entries);
    ht->entries = NULL;
    ht->capacity = 0;
    ht->count = 0;
}

int agent_ht_insert(agent_hash_table_t *ht, const char *key, size_t index)
{
    if (!ht || !ht->entries || !key)
        return AIRY_ERR_INVALID_PARAM;
    if (ht->count >= ht->capacity * 3 / 4)
        return AIRY_ERR_OUT_OF_MEMORY;

    unsigned long h = agent_hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied) {
            ht->entries[pos].key = AIRY_STRDUP(key);
            if (!ht->entries[pos].key)
                return AIRY_ERR_OUT_OF_MEMORY;
            ht->entries[pos].index = index;
            ht->entries[pos].occupied = 1;
            ht->count++;
            return AIRY_SUCCESS;
        }
    }
    return AIRY_ERR_OUT_OF_MEMORY;
}

ssize_t agent_ht_lookup(agent_hash_table_t *ht, const char *key)
{
    if (!ht || !ht->entries || !key || ht->count == 0)
        return -1;

    unsigned long h = agent_hash_fn(key) % ht->capacity;
    for (size_t i = 0; i < ht->capacity; i++) {
        size_t pos = (h + i) % ht->capacity;
        if (!ht->entries[pos].occupied)
            return -1;
        if (strcmp(ht->entries[pos].key, key) == 0)
            return (ssize_t)ht->entries[pos].index;
    }
    return -1;
}

void agent_generate_agent_id(char *buf, size_t buf_size)
{
    /* 32-char hex: 8-char timestamp + 8-char counter + 16-char random.
     * No external libuuid dependency, so the daemon can run standalone. */
    static uint64_t counter = 0;
    static airy_mtx_t counter_lock;
    static int counter_initialized = 0;

    if (!counter_initialized) {
        airy_mtx_init(&counter_lock);
        counter = (uint64_t)time(NULL) & 0xFFFFFFFF;
        counter_initialized = 1;
    }

    airy_mtx_lock(&counter_lock);
    uint64_t c = counter++;
    airy_mtx_unlock(&counter_lock);

    uint64_t t = (uint64_t)time(NULL);

    uint64_t r = t ^ (c * 0x9E3779B97F4A7C15ULL);
    r ^= r << 13;
    r ^= r >> 7;
    r ^= r << 17;

    if (buf_size < AGENT_ID_LEN)
        return;
    snprintf(buf, AGENT_ID_LEN, "%08lx%08lx%016lx", (unsigned long)(t & 0xFFFFFFFFu),
             (unsigned long)(c & 0xFFFFFFFFu), (unsigned long)(r & 0xFFFFFFFFFFFFFFFFULL));
}

/* Monotonic clock in microseconds: POSIX uses CLOCK_MONOTONIC (unaffected by
 * NTP/timezone jumps), Windows uses GetTickCount64 (ms precision converted).
 * Used for spawn/invoke latency aggregation and slow-request detection. */
uint64_t agent_perf_now_us(void)
{
#if AIRY_PLATFORM_POSIX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#else
    return (uint64_t)GetTickCount64() * 1000ull;
#endif
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

    if (agent_ht_init(&svc->agent_index, max_agents * AGENT_HASH_LOAD_FACTOR) != AIRY_SUCCESS) {
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
    agent_ht_destroy(&svc->agent_index);
    svc->agent_count = 0;
    svc->max_agents = 0;
    svc->initialized = 0;
    airy_mtx_unlock(&svc->lock);
    airy_mtx_destroy(&svc->lock);
    airy_mtx_destroy(&svc->session_lock);
    AIRY_FREE(svc);
}
