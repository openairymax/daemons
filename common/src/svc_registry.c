// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_registry.c
 * @brief Service-registry client (registry domain).
 *
 * Implements the cross-process registry interface defined in svc_common.h:
 * service register, unregister, discovery, heartbeat and cleanup. This
 * domain keeps its own g_cross_registry global state, decoupled from the
 * service lifecycle (svc_common.c); discovery/heartbeat access service
 * instances only through the public APIs (airy_svc_get_state/healthcheck/
 * get_stats), never touching service internals.
 *
 * Design principles:
 * 1. Local in-process registry (entries[]), simulating cross-process
 *    registry-client behavior
 * 2. A single global mutex protects all entries, ensuring thread safety
 *    (E-5 concurrency safety)
 * 3. airy_registry_cleanup() is called by airy_svc_common_cleanup()
 *    (svc_common.c) for exit-path cleanup (E-6 traceable errors)
 *
 * @see agentrt/daemons/common/include/svc_common.h
 * @see agentrt/daemons/common/src/svc_common.c
 */

#include "svc_common.h"

#include "airy_memory.h"
#include "daemon_platform_ext.h"
#include "safe_string_utils.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

#define MAX_REGISTRY_ENTRIES 64
#define HEARTBEAT_INTERVAL_MS 30000

typedef struct {
    airy_svc_metadata_t metadata;
    airy_svc_t service;
    bool registered;
    uint64_t register_time;
} registry_entry_t;

static struct {
    char registry_url[512];
    bool initialized;
    registry_entry_t entries[MAX_REGISTRY_ENTRIES];
    uint32_t entry_count;
    airy_mtx_t mutex;
} g_cross_registry = {0};

airy_err_t airy_cross_registry_init(const char *registry_url)
{
    if (!registry_url) {
        return AIRY_EINVAL;
    }

    airy_err_t err = AIRY_SUCCESS;

    err = airy_mtx_init(&g_cross_registry.mutex);
    if (err != AIRY_SUCCESS) {
        return err;
    }

    airy_mtx_lock(&g_cross_registry.mutex);

    if (g_cross_registry.initialized) {
        airy_mtx_unlock(&g_cross_registry.mutex);
        return AIRY_SUCCESS;
    }

    if (safe_strcpy(g_cross_registry.registry_url, registry_url,
                    sizeof(g_cross_registry.registry_url)) != 0) {
        airy_mtx_unlock(&g_cross_registry.mutex);
        return AIRY_EINVAL;
    }
    __builtin_memset(g_cross_registry.entries, 0, sizeof(g_cross_registry.entries));
    g_cross_registry.entry_count = 0;
    g_cross_registry.initialized = true;

    airy_mtx_unlock(&g_cross_registry.mutex);

    AIRY_LOG_INFO("Service registry client initialized: %s", registry_url);
    return AIRY_SUCCESS;
}

airy_err_t airy_registry_register(airy_svc_t service, const airy_svc_metadata_t *metadata)
{
    if (!service || !metadata) {
        return AIRY_EINVAL;
    }

    if (!g_cross_registry.initialized) {
        AIRY_LOG_WARN("Registry not initialized, using local-only registration");
    }

    airy_mtx_lock(&g_cross_registry.mutex);

    if (g_cross_registry.entry_count >= MAX_REGISTRY_ENTRIES) {
        airy_mtx_unlock(&g_cross_registry.mutex);
        AIRY_LOG_ERROR("Registry full, cannot register service '%s'", metadata->name);
        return AIRY_ENOMEM;
    }

    for (uint32_t i = 0; i < g_cross_registry.entry_count; i++) {
        if (g_cross_registry.entries[i].service == service) {
            __builtin_memcpy(&g_cross_registry.entries[i].metadata, metadata,
                             sizeof(airy_svc_metadata_t));
            g_cross_registry.entries[i].metadata.last_heartbeat = airy_time_ms();
            airy_mtx_unlock(&g_cross_registry.mutex);
            AIRY_LOG_INFO("Service '%s' re-registered in cross-process registry", metadata->name);
            return AIRY_SUCCESS;
        }
    }

    registry_entry_t *entry = &g_cross_registry.entries[g_cross_registry.entry_count];
    __builtin_memcpy(&entry->metadata, metadata, sizeof(airy_svc_metadata_t));
    entry->service = service;
    entry->registered = true;
    entry->register_time = airy_time_ms();
    entry->metadata.last_heartbeat = entry->register_time;
    g_cross_registry.entry_count++;

    airy_mtx_unlock(&g_cross_registry.mutex);

    AIRY_LOG_INFO("Service '%s' registered in cross-process registry (type=%s, endpoint=%s)",
             metadata->name, metadata->service_type, metadata->endpoint);
    return AIRY_SUCCESS;
}

airy_err_t airy_registry_deregister(airy_svc_t service)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    airy_mtx_lock(&g_cross_registry.mutex);

    for (uint32_t i = 0; i < g_cross_registry.entry_count; i++) {
        if (g_cross_registry.entries[i].service == service) {
            AIRY_LOG_INFO("Service '%s' deregistered from cross-process registry",
                     g_cross_registry.entries[i].metadata.name);

            if (i < g_cross_registry.entry_count - 1) {
                g_cross_registry.entries[i] =
                    g_cross_registry.entries[g_cross_registry.entry_count - 1];
            }
            __builtin_memset(&g_cross_registry.entries[g_cross_registry.entry_count - 1], 0,
                             sizeof(registry_entry_t));
            g_cross_registry.entry_count--;

            airy_mtx_unlock(&g_cross_registry.mutex);
            return AIRY_SUCCESS;
        }
    }

    airy_mtx_unlock(&g_cross_registry.mutex);
    return AIRY_ENOENT;
}

static bool tag_matches(const char *filter_tags, const char *service_tags)
{
    if (!filter_tags || !filter_tags[0])
        return true;
    if (!service_tags || !service_tags[0])
        return false;

    char filter_copy[AIRY_MAX_TAGS_LEN];
    if (safe_strcpy(filter_copy, filter_tags, sizeof(filter_copy)) != 0) {
        return false;
    }

    char *saveptr = NULL;
    char *token = strtok_r(filter_copy, ",", &saveptr);
    while (token) {
        while (*token == ' ')
            token++;
        if (strstr(service_tags, token)) {
            return true;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    return false;
}

airy_svc_metadata_t *airy_registry_discover(const char *service_type, const char *filter_tags,
                                            size_t *result_count)
{
    if (!result_count) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    *result_count = 0;

    if (!g_cross_registry.initialized && g_cross_registry.entry_count == 0) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_mtx_lock(&g_cross_registry.mutex);

    size_t match_count = 0;
    for (uint32_t i = 0; i < g_cross_registry.entry_count; i++) {
        registry_entry_t *entry = &g_cross_registry.entries[i];
        if (!entry->registered)
            continue;

        bool type_match = !service_type || !service_type[0] ||
                          strcmp(entry->metadata.service_type, service_type) == 0;
        bool tag_match = tag_matches(filter_tags, entry->metadata.tags);

        if (type_match && tag_match) {
            match_count++;
        }
    }

    if (match_count == 0) {
        airy_mtx_unlock(&g_cross_registry.mutex);
        AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
    }

    airy_svc_metadata_t *results =
        (airy_svc_metadata_t *)AIRY_CALLOC(match_count, sizeof(airy_svc_metadata_t));
    if (!results) {
        airy_mtx_unlock(&g_cross_registry.mutex);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    size_t idx = 0;
    for (uint32_t i = 0; i < g_cross_registry.entry_count && idx < match_count; i++) {
        registry_entry_t *entry = &g_cross_registry.entries[i];
        if (!entry->registered)
            continue;

        bool type_match = !service_type || !service_type[0] ||
                          strcmp(entry->metadata.service_type, service_type) == 0;
        bool tag_match = tag_matches(filter_tags, entry->metadata.tags);

        if (type_match && tag_match) {
            __builtin_memcpy(&results[idx], &entry->metadata, sizeof(airy_svc_metadata_t));
            idx++;
        }
    }

    *result_count = match_count;
    airy_mtx_unlock(&g_cross_registry.mutex);

    AIRY_LOG_DEBUG("Service discovery: found %zu services (type=%s, tags=%s)", match_count,
              service_type ? service_type : "*", filter_tags ? filter_tags : "*");
    return results;
}

void airy_registry_discover_free(airy_svc_metadata_t *results)
{
    if (results) {
        AIRY_FREE(results);
    }
}

airy_err_t airy_registry_heartbeat(airy_svc_t service)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    airy_mtx_lock(&g_cross_registry.mutex);

    for (uint32_t i = 0; i < g_cross_registry.entry_count; i++) {
        if (g_cross_registry.entries[i].service == service) {
            g_cross_registry.entries[i].metadata.last_heartbeat = airy_time_ms();
            g_cross_registry.entries[i].metadata.state = airy_svc_get_state(service);
            g_cross_registry.entries[i].metadata.healthy =
                (airy_svc_healthcheck(service) == AIRY_SUCCESS);

            airy_svc_stats_t stats;
            if (airy_svc_get_stats(service, &stats) == AIRY_SUCCESS) {
                g_cross_registry.entries[i].metadata.current_load =
                    stats.current_concurrent > 0 ?
                        (uint32_t)(stats.current_concurrent * 100 /
                                   (stats.peak_concurrent > 0 ? stats.peak_concurrent : 1)) :
                        0;
            }

            airy_mtx_unlock(&g_cross_registry.mutex);
            return AIRY_SUCCESS;
        }
    }

    airy_mtx_unlock(&g_cross_registry.mutex);
    return AIRY_ENOENT;
}

void airy_registry_cleanup(void)
{
    airy_mtx_lock(&g_cross_registry.mutex);

    __builtin_memset(g_cross_registry.entries, 0, sizeof(g_cross_registry.entries));
    g_cross_registry.entry_count = 0;
    g_cross_registry.initialized = false;
    g_cross_registry.registry_url[0] = '\0';

    airy_mtx_unlock(&g_cross_registry.mutex);
    airy_mtx_destroy(&g_cross_registry.mutex);

    AIRY_LOG_INFO("Service registry client cleaned up");
}
