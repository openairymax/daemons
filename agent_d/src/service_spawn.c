// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_spawn.c
 * @brief Agent service spawn domain: spawns the agent child, verifies the
 *        readiness handshake and registers in the hash table (with
 *        AIRY_AGENT_NO_SPAWN deterministic-mode support).
 */

#include "airy_memory.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if AIRY_PLATFORM_POSIX
#include <sys/select.h>
#include <sys/wait.h>
#endif

#include "agent_service_internal.h"

/* Find a free agent slot under the global lock; on failure the lock is
 * already released and the error is returned to the caller. */
static int agent_spawn_acquire_slot(agent_service_t *svc, agent_entry_internal_t **out_agent,
                                    size_t *out_idx)
{
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < svc->agent_count; i++) {
        if (svc->agents[i].status == AGENT_STATUS_FREE) {
            idx = i;
            break;
        }
    }
    if (idx == SIZE_MAX) {
        if (svc->agent_count >= svc->max_agents) {
            airy_mtx_unlock(&svc->lock);
            SVC_LOG_WARN("Agent service full (count=%zu)", svc->agent_count);
            airy_atomic_fetch_add(&svc->m_spawn_fail, 1);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        idx = svc->agent_count++;
    }
    *out_agent = &svc->agents[idx];
    *out_idx = idx;
    return AIRY_SUCCESS;
}

/* Allocate agent id/spec and mark the slot SPAWNING; on failure the lock is
 * already released and the error is returned. */
static int agent_spawn_init_entry(agent_service_t *svc, agent_entry_internal_t *agent,
                                  const char *spec)
{
    char id_buf[AGENT_ID_LEN];
    agent_generate_agent_id(id_buf, sizeof(id_buf));
    agent->agent_id = AIRY_STRDUP(id_buf);
    agent->spec = AIRY_STRDUP(spec);
    if (!agent->agent_id || !agent->spec) {
        AIRY_FREE(agent->agent_id);
        AIRY_FREE(agent->spec);
        agent->agent_id = NULL;
        agent->spec = NULL;
        agent->status = AGENT_STATUS_FREE;
        airy_mtx_unlock(&svc->lock);
        airy_atomic_fetch_add(&svc->m_spawn_fail, 1);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    agent->status = AGENT_STATUS_SPAWNING;
    agent->spawned_at = (uint64_t)time(NULL);
    airy_mtx_unlock(&svc->lock);
    return AIRY_SUCCESS;
}

/* Read the child's readiness handshake; a not-ready child is killed and
 * reaped and child_pid is reset to -1. */
#if AIRY_PLATFORM_POSIX
static void agent_spawn_verify_child(const char *agent_id, pid_t *child_pid, int *child_sin,
                                     int *child_sout)
{
    char ready_buf[512];
    int ready_rc = agent_read_line_timeout(*child_sout, ready_buf, sizeof(ready_buf),
                                           agent_spawn_ready_timeout_s());
    int alive = 0;
    if (ready_rc == 0) {
        cJSON *r = cJSON_Parse(ready_buf);
        if (r) {
            cJSON *ready_item = cJSON_GetObjectItem(r, "ready");
            if (ready_item && cJSON_IsTrue(ready_item))
                alive = 1;
            cJSON_Delete(r);
        }
    }
    if (alive) {
        SVC_LOG_INFO("Agent child spawned: agent_id=%s, pid=%d", agent_id, (int)*child_pid);
    } else {
        SVC_LOG_WARN("Agent child not ready, spawn rejected: agent_id=%s, resp=%s", agent_id,
                     ready_rc == 0 ? ready_buf : "(timeout/eof)");
        pid_t dead_pid = *child_pid;
        int dead_sin = *child_sin, dead_sout = *child_sout;
        agent_kill_and_reap(&dead_pid, &dead_sin, &dead_sout);
        *child_pid = -1;
    }
}
#endif /* AIRY_PLATFORM_POSIX */

/* Register the spawned agent in the hash table on success, or roll back to
 * FREE on failure; returns the final status of the spawn operation. */
static int agent_spawn_finish(agent_service_t *svc, agent_entry_internal_t *agent, size_t idx,
                              int spawn_ok, pid_t child_pid, int child_sin, int child_sout,
                              char **out_agent_id, uint64_t perf_t0)
{
    airy_mtx_lock(&agent->entry_lock);

    if (spawn_ok) {
#if AIRY_PLATFORM_POSIX
        if (child_pid > 0) {
            agent->child_pid = child_pid;
            agent->stdin_fd = child_sin;
            agent->stdout_fd = child_sout;
            agent->last_active = (uint64_t)time(NULL);
        }
#endif
        int rc = AIRY_SUCCESS;
        agent_lock_svc(svc);
        if (svc->initialized) {
            rc = agent_ht_insert(&svc->agent_index, agent->agent_id, idx);
        }
        if (rc == AIRY_SUCCESS) {
            agent->status = AGENT_STATUS_RUNNING;
            airy_mtx_unlock(&svc->lock);
            airy_mtx_unlock(&agent->entry_lock);
            *out_agent_id = AIRY_STRDUP(agent->agent_id);

            airy_atomic_fetch_add(&svc->m_spawn_ok, 1);
            agent_perf_accumulate(&svc->m_spawn_us_total, &svc->m_spawn_us_max,
                                  agent_perf_now_us() - perf_t0);
            SVC_LOG_DEBUG("Agent spawn: agent_id=%s, total=%lu", *out_agent_id,
                          (unsigned long)svc->agent_count);
            return AIRY_SUCCESS;
        }

#if AIRY_PLATFORM_POSIX
        if (agent->child_pid > 0) {
            agent_kill_and_reap(&agent->child_pid, &agent->stdin_fd, &agent->stdout_fd);
        }
#endif
        airy_mtx_unlock(&svc->lock);
    }

    /* Failure path: mark idle and release resources (agent_id/spec must be
     * freed under the global lock, consistent with list's concurrent reads) */
    agent->status = AGENT_STATUS_FREE;
    airy_mtx_unlock(&agent->entry_lock);

    agent_lock_svc(svc);
    AIRY_FREE(agent->agent_id);
    AIRY_FREE(agent->spec);
    agent->agent_id = NULL;
    agent->spec = NULL;
    airy_mtx_unlock(&svc->lock);

    airy_atomic_fetch_add(&svc->m_spawn_fail, 1);
    agent_perf_accumulate(&svc->m_spawn_us_total, &svc->m_spawn_us_max,
                          agent_perf_now_us() - perf_t0);

    return spawn_ok ? AIRY_ERR_FAIL : AIRY_ERR_SVC_NOT_READY;
}

int agent_service_spawn(agent_service_t *svc, const char *spec, char **out_agent_id)
{
    if (!svc || !svc->initialized || !spec || spec[0] == '\0' || !out_agent_id)
        return AIRY_ERR_INVALID_PARAM;

    *out_agent_id = NULL;

    uint64_t perf_t0 = agent_perf_now_us();
    airy_atomic_fetch_add(&svc->m_spawn_total, 1);

    agent_lock_svc(svc);

    size_t idx = SIZE_MAX;
    agent_entry_internal_t *agent = NULL;
    int rc = agent_spawn_acquire_slot(svc, &agent, &idx);
    if (rc != AIRY_SUCCESS)
        return rc;

    rc = agent_spawn_init_entry(svc, agent, spec);
    if (rc != AIRY_SUCCESS)
        return rc;

    pid_t child_pid = -1;
    int child_sin = -1, child_sout = -1;
    int spawn_ok = 1;

#if AIRY_PLATFORM_POSIX
    /* P0-2: really spawn and verify the child is alive, no silent stub
     * fallback. AIRY_AGENT_NO_SPAWN=1 skips fork (deterministic mode for unit
     * tests): registration is treated as success but there is no child, and
     * invoke returns a clear error instead of fake success. */
    const char *no_spawn_env = getenv("AIRY_AGENT_NO_SPAWN");
    int spawn_disabled =
        (no_spawn_env && no_spawn_env[0] != '\0' && strcmp(no_spawn_env, "0") != 0);

    if (spawn_disabled) {
        SVC_LOG_WARN("Agent spawn skipped (AIRY_AGENT_NO_SPAWN): agent_id=%s", agent->agent_id);
    } else if (agent_spawn_child(spec, agent->agent_id, &child_pid, &child_sin, &child_sout) == 0) {
        agent_spawn_verify_child(agent->agent_id, &child_pid, &child_sin, &child_sout);
    } else {
        SVC_LOG_WARN("Agent child spawn failed: agent_id=%s", agent->agent_id);
    }

    if (child_pid <= 0 && !spawn_disabled) {
        spawn_ok = 0;
    }
#endif

    return agent_spawn_finish(svc, agent, idx, spawn_ok, child_pid, child_sin, child_sout,
                              out_agent_id, perf_t0);
}
