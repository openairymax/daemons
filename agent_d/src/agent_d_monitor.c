// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_d_monitor.c
 * @brief agent_d 后台监控线程域（POSIX）：空闲 agent 子进程回收线程与
 *        周期性 [PERF] 采样线程，以及共享的微秒计时工具 perf_now_us() /
 *        perf_slow_threshold_us()。
 *
 * 2026-08-27 域拆分（原 main.c 826 行 → 3 文件）：入口引导见 main.c，
 * RPC 方法见 agent_d_rpc.c。
 */

#include "airy_memory.h"
#include "error.h"
#include "agent_d_internal.h"

#include "daemon_main.h"
#include "agent_service.h"
#include "platform.h"
#include "svc_logger.h"
#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if AIRY_PLATFORM_POSIX
/* P0-3: idle agent child reaping.
 * A guardian thread periodically calls agent_service_reap_idle, terminating
 * child processes with no calls beyond the idle threshold
 * (AIRY_AGENT_IDLE_TIMEOUT_S, default 300s), preventing Python runner process
 * leaks (historically up to 12 idle processes remained). */
static volatile int g_reaper_run = 0;
static airy_thread_t g_reaper_thread = AIRY_INVALID_THREAD;

static void *idle_reaper_thread(void *arg)
{
    (void)arg;
    const char *env_timeout = getenv("AIRY_AGENT_IDLE_TIMEOUT_S");
    uint64_t max_idle_s = 300;
    if (env_timeout && env_timeout[0] != '\0') {
        unsigned long long v = strtoull(env_timeout, NULL, 10);
        if (v > 0)
            max_idle_s = (uint64_t)v;
    }
    SVC_LOG_INFO("Idle reaper started (max_idle=%llus, scan every %ds)",
                 (unsigned long long)max_idle_s, 30);
    int slept = 0;
    while (g_reaper_run) {

        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        if (!g_reaper_run)
            break;
        if (++slept >= 30) {
            slept = 0;
            if (g_service)
                agent_service_reap_idle(g_service, max_idle_s);
        }
    }
    return NULL;
}

void idle_reaper_start(void)
{
    g_reaper_run = 1;
    if (airy_thread_create(&g_reaper_thread, idle_reaper_thread, NULL) != 0) {
        g_reaper_run = 0;
        SVC_LOG_WARN("Failed to start idle reaper thread");
    }
}

void idle_reaper_stop(void)
{
    g_reaper_run = 0;
    if (g_reaper_thread != AIRY_INVALID_THREAD) {
        airy_thread_join(g_reaper_thread, NULL);
        g_reaper_thread = AIRY_INVALID_THREAD;
    }
}
#endif /* AIRY_PLATFORM_POSIX */

uint64_t perf_now_us(void)
{
#if AIRY_PLATFORM_POSIX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#else
    return (uint64_t)GetTickCount64() * 1000ull;
#endif
}

int64_t perf_slow_threshold_us(void)
{
    const char *env = getenv("AIRY_AGENT_PERF_SLOW_US");
    if (env && env[0] != '\0') {
        long long v = strtoll(env, NULL, 10);
        if (v > 0)
            return v;
    }
    return 1000000;
}

#if AIRY_PLATFORM_POSIX
/* Periodic sampling thread: aggregates the service-layer atomic counters +
 * thread-pool state into a one-line [PERF] summary. With 10000-way concurrency,
 * per-request logging would flood IO, so only window aggregation is done:
 *   - spawn/invoke/terminate window deltas and cumulative success/failure
 *   - spawn/invoke average/max latency (microseconds)
 *   - global-lock contention count (lock_wait, trylock probing)
 *   - current agent count / peak concurrency / thread-pool active and pending
 *     (queue depth)
 * Sampling interval: AIRY_AGENT_PERF_INTERVAL_S, default 5s. */
static volatile int g_perf_run = 0;
static airy_thread_t g_perf_thread = AIRY_INVALID_THREAD;
static daemon_event_driver_t *g_perf_driver = NULL;

static void *perf_monitor_thread(void *arg)
{
    (void)arg;
    const char *env_interval = getenv("AIRY_AGENT_PERF_INTERVAL_S");
    int interval_s = 5;
    if (env_interval && env_interval[0] != '\0') {
        long v = strtol(env_interval, NULL, 10);
        if (v > 0 && v <= 3600)
            interval_s = (int)v;
    }
    SVC_LOG_INFO("Perf monitor started (interval=%ds, slow_threshold_us=%lld)", interval_s,
                 (long long)perf_slow_threshold_us());

    agent_perf_stats_t prev;
    __builtin_memset(&prev, 0, sizeof(prev));
    int slept = 0;
    while (g_perf_run) {
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        if (!g_perf_run)
            break;
        if (++slept < interval_s)
            continue;
        slept = 0;

        if (!g_service)
            continue;
        agent_perf_stats_t cur;
        if (agent_service_get_perf(g_service, &cur) != AIRY_SUCCESS)
            continue;

        int d_spawn = cur.spawn_total - prev.spawn_total;
        int d_invoke = cur.invoke_total - prev.invoke_total;
        int d_terminate = cur.terminate_total - prev.terminate_total;
        int d_lock = cur.lock_wait_total - prev.lock_wait_total;
        prev = cur;

        uint32_t pool_active = 0, pool_pending = 0;
        thread_pool_t *pool =
            g_perf_driver ? daemon_event_driver_get_pool(g_perf_driver) : NULL;
        if (pool) {
            pool_active = thread_pool_active_count(pool);
            pool_pending = thread_pool_pending_count(pool);
        }

        uint64_t spawn_avg = (cur.spawn_ok > 0) ?
                                 (uint64_t)(cur.spawn_us_total / (unsigned long long)cur.spawn_ok) :
                                 0;
        uint64_t invoke_avg =
            (cur.invoke_ok > 0) ?
                (uint64_t)(cur.invoke_us_total / (unsigned long long)cur.invoke_ok) :
                0;

        SVC_LOG_INFO("[PERF] window=%ds spawn{+%d total=%d ok=%d fail=%d avg_us=%llu max_us=%llu} "
                     "invoke{+%d total=%d ok=%d fail=%d avg_us=%llu max_us=%llu} "
                     "terminate{+%d total=%d} lock_wait{+%d total=%d} "
                     "agents=%zu/%zu peak_running=%d pool{active=%u pending=%u}",
                     interval_s, d_spawn, cur.spawn_total, cur.spawn_ok, cur.spawn_fail,
                     (unsigned long long)spawn_avg, cur.spawn_us_max, d_invoke, cur.invoke_total,
                     cur.invoke_ok, cur.invoke_fail, (unsigned long long)invoke_avg,
                     cur.invoke_us_max, d_terminate, cur.terminate_total, d_lock,
                     cur.lock_wait_total, agent_service_count(g_service), g_config.max_agents,
                     cur.peak_running, pool_active, pool_pending);
    }
    return NULL;
}

void perf_monitor_start(daemon_event_driver_t *driver)
{
    g_perf_run = 1;
    g_perf_driver = driver;
    if (airy_thread_create(&g_perf_thread, perf_monitor_thread, NULL) != 0) {
        g_perf_run = 0;
        g_perf_driver = NULL;
        SVC_LOG_WARN("Failed to start perf monitor thread");
    }
}

void perf_monitor_stop(void)
{
    g_perf_run = 0;
    if (g_perf_thread != AIRY_INVALID_THREAD) {
        airy_thread_join(g_perf_thread, NULL);
        g_perf_thread = AIRY_INVALID_THREAD;
    }
    g_perf_driver = NULL;
}
#endif /* AIRY_PLATFORM_POSIX */
