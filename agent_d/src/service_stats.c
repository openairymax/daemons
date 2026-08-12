// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_stats.c
 * @brief Agent 服务状态管理域：运行中 agent 列表/计数、空闲回收与
 *        性能统计导出
 */

#include "airy_memory.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if AIRY_PLATFORM_POSIX
#include <sys/select.h>
#include <sys/wait.h>
#endif

#include "agent_service_internal.h"

int agent_service_list(agent_service_t *svc, char ***out_agent_ids, size_t *out_count)
{
    if (!svc || !svc->initialized || !out_agent_ids || !out_count)
        return AIRY_ERR_INVALID_PARAM;

    *out_agent_ids = NULL;
    *out_count = 0;

    agent_lock_svc(svc);

    if (svc->agent_count == 0) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_SUCCESS;
    }

    char **ids = (char **)AIRY_CALLOC(svc->agent_count > 0 ? svc->agent_count : 1, sizeof(char *));
    if (!ids) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    size_t collected = 0;
    for (size_t i = 0; i < svc->agent_count; i++) {
        if (svc->agents[i].status != AGENT_STATUS_RUNNING)
            continue;
        ids[collected] = AIRY_STRDUP(svc->agents[i].agent_id);
        if (!ids[collected]) {
            for (size_t j = 0; j < collected; j++)
                AIRY_FREE(ids[j]);
            AIRY_FREE(ids);
            airy_mtx_unlock(&svc->lock);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        collected++;
    }

    *out_agent_ids = ids;
    *out_count = collected;

    airy_mtx_unlock(&svc->lock);

    SVC_LOG_DEBUG("Agent list: count=%zu", *out_count);
    return AIRY_SUCCESS;
}

size_t agent_service_count(agent_service_t *svc)
{
    if (!svc || !svc->initialized)
        return 0;
    agent_lock_svc(svc);
    size_t c = svc->agent_count;
    airy_mtx_unlock(&svc->lock);
    return c;
}

int agent_service_reap_idle(agent_service_t *svc, uint64_t max_idle_s)
{
    if (!svc || !svc->initialized)
        return AIRY_ERR_INVALID_PARAM;

    uint64_t now = (uint64_t)time(NULL);
    size_t reaped = 0;

    /* Under the global lock only collect idle slot indexes (fast path); kill
     * runs outside the lock to avoid blocking other agent operations */
    size_t *candidates = NULL;
    size_t candidate_count = 0;
    {
        agent_lock_svc(svc);
        if (svc->agent_count > 0) {

            if (svc->agent_count > SIZE_MAX / sizeof(size_t)) {
                airy_mtx_unlock(&svc->lock);
                return AIRY_ERR_OUT_OF_MEMORY;
            }
            candidates = (size_t *)AIRY_MALLOC(svc->agent_count * sizeof(size_t));
            if (candidates) {
                for (size_t i = 0; i < svc->agent_count; i++) {
                    agent_entry_internal_t *a = &svc->agents[i];
                    if (a->status != AGENT_STATUS_RUNNING || a->child_pid <= 0)
                        continue;
                    if (now <= a->last_active || (now - a->last_active) < max_idle_s)
                        continue;
                    candidates[candidate_count++] = i;
                }
            }
        }
        airy_mtx_unlock(&svc->lock);
    }

#if AIRY_PLATFORM_POSIX
    for (size_t j = 0; j < candidate_count; j++) {
        agent_entry_internal_t *a = &svc->agents[candidates[j]];
        airy_mtx_lock(&a->entry_lock);
        if (a->status == AGENT_STATUS_RUNNING && a->child_pid > 0) {
            SVC_LOG_INFO("Agent idle reclaimed: agent_id=%s, idle=%llus", a->agent_id,
                         (unsigned long long)(now - a->last_active));
            agent_kill_and_reap(&a->child_pid, &a->stdin_fd, &a->stdout_fd);
            a->status = AGENT_STATUS_TERMINATED;
            reaped++;
        }
        airy_mtx_unlock(&a->entry_lock);
    }
#endif
    AIRY_FREE(candidates);

    if (reaped > 0)
        SVC_LOG_INFO("Agent idle reclaim done: reaped=%zu", reaped);
    return AIRY_SUCCESS;
}

void agent_service_list_free(char **agent_ids, size_t count)
{
    if (!agent_ids)
        return;
    for (size_t i = 0; i < count; i++) {
        AIRY_FREE(agent_ids[i]);
    }
    AIRY_FREE(agent_ids);
}

int agent_service_get_perf(agent_service_t *svc, agent_perf_stats_t *out)
{
    if (!svc || !out)
        return AIRY_ERR_INVALID_PARAM;

    out->spawn_total = airy_atomic_load(&svc->m_spawn_total);
    out->spawn_ok = airy_atomic_load(&svc->m_spawn_ok);
    out->spawn_fail = airy_atomic_load(&svc->m_spawn_fail);
    out->invoke_total = airy_atomic_load(&svc->m_invoke_total);
    out->invoke_ok = airy_atomic_load(&svc->m_invoke_ok);
    out->invoke_fail = airy_atomic_load(&svc->m_invoke_fail);
    out->terminate_total = airy_atomic_load(&svc->m_terminate_total);
    out->lock_wait_total = airy_atomic_load(&svc->m_lock_wait_total);
    out->spawn_us_total =
        (unsigned long long)atomic_load_explicit(&svc->m_spawn_us_total, memory_order_relaxed);
    out->spawn_us_max =
        (unsigned long long)atomic_load_explicit(&svc->m_spawn_us_max, memory_order_relaxed);
    out->invoke_us_total =
        (unsigned long long)atomic_load_explicit(&svc->m_invoke_us_total, memory_order_relaxed);
    out->invoke_us_max =
        (unsigned long long)atomic_load_explicit(&svc->m_invoke_us_max, memory_order_relaxed);

    /* Peak concurrency: scan the running slot count and CAS-update the peak.
     * The O(n) scan runs periodically from the monitor thread (default 5s);
     * the 10000-slot cost is negligible */
    size_t running = 0;
    {
        agent_lock_svc(svc);
        for (size_t i = 0; i < svc->agent_count; i++) {
            if (svc->agents[i].status == AGENT_STATUS_RUNNING)
                running++;
        }
        airy_mtx_unlock(&svc->lock);
    }
    int prev_peak = airy_atomic_load(&svc->m_peak_running);
    while ((int)running > prev_peak) {
        if (atomic_compare_exchange_weak_explicit(&svc->m_peak_running, &prev_peak, (int)running,
                                                  memory_order_relaxed, memory_order_relaxed))
            break;
    }
    out->peak_running = airy_atomic_load(&svc->m_peak_running);

    return AIRY_SUCCESS;
}
