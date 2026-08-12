// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_auth_ratelimit.c
 * @brief Daemon 认证中间件限流域：令牌桶速率限制器的初始化/检查/
 *        重置/统计/清理
 */

#include "airy_memory.h"
#include "error.h"
#include "svc_auth.h"
#include "svc_logger.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "svc_auth_internal.h"

ratelimit_global_state_t g_ratelimit = {.initialized = 0};

int auth_ratelimit_init(const rate_limit_config_t *config)
{
    if (g_ratelimit.initialized)
        return AIRY_ERR_ALREADY_INIT;

    airy_mtx_init(&g_ratelimit.lock);

    if (config) {
        __builtin_memcpy(&g_ratelimit.config, config, sizeof(rate_limit_config_t));
    } else {
        g_ratelimit.config.requests_per_sec = DEFAULT_RPS;
        g_ratelimit.config.burst_size = DEFAULT_BURST_SIZE;
        g_ratelimit.config.max_clients = MAX_CLIENTS;
    }

    __builtin_memset(g_ratelimit.entries, 0, sizeof(g_ratelimit.entries));
    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        g_ratelimit.entries[i].active = false;
    }

    g_ratelimit.initialized = 1;
    SVC_LOG_INFO("Rate limiter initialized (rps=%u, burst=%u)", g_ratelimit.config.requests_per_sec,
                 g_ratelimit.config.burst_size);
    return AUTH_SUCCESS;
}

int auth_ratelimit_check(const char *client_id)
{
    if (!g_ratelimit.initialized || !client_id) {
        return AUTH_RATE_LIMIT_EXCEEDED;
    }

    airy_mtx_lock(&g_ratelimit.lock);

    time_t now = time(NULL);
    rate_limit_entry_t *entry = NULL;
    size_t free_slot = SIZE_MAX;

    for (size_t i = 0; i < g_ratelimit.config.max_clients; i++) {
        if (g_ratelimit.entries[i].active) {
            if (strncmp(g_ratelimit.entries[i].client_id, client_id,
                        sizeof(g_ratelimit.entries[i].client_id) - 1) == 0) {
                entry = &g_ratelimit.entries[i];
                break;
            }
        } else if (free_slot == SIZE_MAX) {
            free_slot = i;
        }
    }

    if (!entry && free_slot != SIZE_MAX) {
        entry = &g_ratelimit.entries[free_slot];
        AIRY_STRNCPY_TERM(entry->client_id, client_id, sizeof(entry->client_id));
        entry->client_id[sizeof(entry->client_id) - 1] = '\0';
        entry->max_tokens = (double)g_ratelimit.config.burst_size;
        entry->tokens = entry->max_tokens;
        entry->refill_rate = (double)g_ratelimit.config.requests_per_sec;
        entry->last_update = now;
        entry->active = true;
    }

    if (!entry) {
        time_t oldest_time = now;
        size_t oldest_idx = 0;

        for (size_t i = 0; i < g_ratelimit.config.max_clients; i++) {
            if (g_ratelimit.entries[i].active && g_ratelimit.entries[i].last_update < oldest_time) {
                oldest_time = g_ratelimit.entries[i].last_update;
                oldest_idx = i;
            }
        }

        entry = &g_ratelimit.entries[oldest_idx];
        SVC_LOG_DEBUG("Rate limit: evicting stale client: %s", entry->client_id);
        AIRY_STRNCPY_TERM(entry->client_id, client_id, sizeof(entry->client_id));
        entry->client_id[sizeof(entry->client_id) - 1] = '\0';
        entry->max_tokens = (double)g_ratelimit.config.burst_size;
        entry->tokens = entry->max_tokens;
        entry->refill_rate = (double)g_ratelimit.config.requests_per_sec;
        entry->last_update = now;
    }

    double elapsed = difftime(now, entry->last_update);
    entry->tokens += elapsed * entry->refill_rate;
    if (entry->tokens > entry->max_tokens) {
        entry->tokens = entry->max_tokens;
    }
    entry->last_update = now;

    if (entry->tokens >= 1.0) {
        entry->tokens -= 1.0;
        airy_mtx_unlock(&g_ratelimit.lock);
        return AUTH_SUCCESS;
    }

    airy_mtx_unlock(&g_ratelimit.lock);
    SVC_LOG_DEBUG("Rate limit exceeded for client: %s", client_id);
    return AUTH_RATE_LIMIT_EXCEEDED;
}

int auth_ratelimit_reset(const char *client_id)
{
    if (!g_ratelimit.initialized || !client_id) {
        return AUTH_RATE_LIMIT_EXCEEDED;
    }

    airy_mtx_lock(&g_ratelimit.lock);

    for (size_t i = 0; i < g_ratelimit.config.max_clients; i++) {
        if (g_ratelimit.entries[i].active &&
            strncmp(g_ratelimit.entries[i].client_id, client_id,
                    sizeof(g_ratelimit.entries[i].client_id) - 1) == 0) {
            g_ratelimit.entries[i].tokens = g_ratelimit.entries[i].max_tokens;
            g_ratelimit.entries[i].last_update = time(NULL);
            airy_mtx_unlock(&g_ratelimit.lock);
            return AUTH_SUCCESS;
        }
    }

    airy_mtx_unlock(&g_ratelimit.lock);
    return AUTH_SUCCESS;
}

int auth_ratelimit_get_stats(const char *client_id, uint32_t *remaining, int64_t *reset_time)
{
    if (!g_ratelimit.initialized || !client_id || !remaining) {
        return AUTH_RATE_LIMIT_EXCEEDED;
    }

    airy_mtx_lock(&g_ratelimit.lock);

    for (size_t i = 0; i < g_ratelimit.config.max_clients; i++) {
        if (g_ratelimit.entries[i].active &&
            strncmp(g_ratelimit.entries[i].client_id, client_id,
                    sizeof(g_ratelimit.entries[i].client_id) - 1) == 0) {
            *remaining = (uint32_t)g_ratelimit.entries[i].tokens;
            if (reset_time) {
                *reset_time = (int64_t)g_ratelimit.entries[i].last_update * 1000;
            }
            airy_mtx_unlock(&g_ratelimit.lock);
            return AUTH_SUCCESS;
        }
    }

    airy_mtx_unlock(&g_ratelimit.lock);
    return AUTH_RATE_LIMIT_EXCEEDED;
}

void auth_ratelimit_cleanup(void)
{
    if (g_ratelimit.initialized) {
        airy_mtx_lock(&g_ratelimit.lock);
        __builtin_memset(g_ratelimit.entries, 0, sizeof(g_ratelimit.entries));
        airy_mtx_unlock(&g_ratelimit.lock);
        airy_mtx_destroy(&g_ratelimit.lock);
        g_ratelimit.initialized = 0;
        SVC_LOG_INFO("Rate limiter cleaned up");
    }
}
