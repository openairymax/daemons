// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_common_registry.c
 * @brief Common service implementation - in-process service registry domain.
 *
 * Phase 2.3a split from svc_common.c: owns the global service registry
 * state (g_registry) and every function that enumerates or mutates it:
 * - module lazy init/cleanup (registry mutex + memory stats reporter)
 * - internal register/unregister/lookup helpers used by the lifecycle
 *   domain (svc_common.c) during airy_svc_create()/airy_svc_destroy()
 * - public registry API: airy_svc_register/unregister/find/count/foreach
 * - process-exit cleanup entry point airy_svc_common_cleanup()
 *
 * The public API surface (svc_common.h) is unchanged by this split.
 *
 * @see agentrt/daemons/common/src/svc_common.c (lifecycle domain)
 * @see agentrt/daemons/common/src/svc_common_ops.c (query/async domain)
 * @see agentrt/daemons/common/src/svc_common_internal.h
 */

#include "svc_common.h"
#include "daemon_errors.h"
#include "svc_common_internal.h"

#include "memory_stats_reporter.h"
#include "svc_logger.h"

#include <string.h>

/**
 * @brief Service registry internal state (private to this domain)
 */
static struct {
    airy_svc_internal_t *services;
    airy_mtx_t registry_mutex;
    uint32_t service_count;
    int initialized;
} g_registry = {.services = NULL, .service_count = 0, .initialized = 0};

/**
 * @brief Initialize the service management module
 */
airy_err_t svc_common_module_init(void)
{
    if (g_registry.initialized) {
        return AIRY_SUCCESS;
    }

    airy_err_t err = AIRY_SUCCESS;

    err = airy_mtx_init(&g_registry.registry_mutex);
    if (err != AIRY_SUCCESS) {
        AIRY_LOG_ERROR("Failed to initialize registry mutex: %d", err);
        AIRY_ERROR(DAEMON_EINIT, "svc_common: registry mutex init failed");
    }

    g_registry.initialized = 1;

    /* Initialize memory stats reporter (SEC-15) */
    airy_mem_stats_reporter_init();

    AIRY_LOG_DEBUG("Service common module initialized");

    return AIRY_SUCCESS;
}

/**
 * @brief Clean up the service management module
 */
static void svc_common_module_cleanup(void)
{
    if (!g_registry.initialized) {
        return;
    }

    airy_mtx_destroy(&g_registry.registry_mutex);
    g_registry.initialized = 0;

    AIRY_LOG_DEBUG("Service common module cleaned up");
}

/**
 * @brief Find a service in the internal registry
 */
static airy_svc_internal_t *find_service_internal(const char *name)
{
    if (!name || !g_registry.initialized) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_svc_internal_t *current = g_registry.services;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }

    AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "operation failed");
}

/**
 * @brief Register a service into the internal registry
 */
airy_err_t register_service_internal(airy_svc_internal_t *service)
{
    if (!service || !g_registry.initialized) {
        AIRY_ERROR(AIRY_EINVAL, "register_service_internal: null service");
    }

    airy_mtx_lock(&g_registry.registry_mutex);

    if (find_service_internal(service->name)) {
        airy_mtx_unlock(&g_registry.registry_mutex);
        return AIRY_EEXIST;
    }

    service->next = g_registry.services;
    g_registry.services = service;
    g_registry.service_count++;

    airy_mtx_unlock(&g_registry.registry_mutex);

    AIRY_LOG_INFO("Service '%s' registered internally", service->name);

    return AIRY_SUCCESS;
}

/**
 * @brief Unregister a service from the internal registry
 */
airy_err_t unregister_service_internal(airy_svc_internal_t *service)
{
    if (!service || !g_registry.initialized) {
        return AIRY_EINVAL;
    }

    airy_mtx_lock(&g_registry.registry_mutex);

    airy_svc_internal_t **prev = &g_registry.services;
    airy_svc_internal_t *current = g_registry.services;

    while (current) {
        if (current == service) {
            *prev = current->next;
            g_registry.service_count--;

            airy_mtx_unlock(&g_registry.registry_mutex);
            AIRY_LOG_INFO("Service '%s' unregistered internally", service->name);
            return AIRY_SUCCESS;
        }

        prev = &current->next;
        current = current->next;
    }

    airy_mtx_unlock(&g_registry.registry_mutex);

    return AIRY_ENOENT;
}

airy_err_t airy_svc_register(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    if (!g_registry.initialized) {
        airy_err_t err = svc_common_module_init();
        if (err != AIRY_SUCCESS) {
            return err;
        }
    }

    airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;
    airy_mtx_lock(&g_registry.registry_mutex);

    for (airy_svc_internal_t *current = g_registry.services; current; current = current->next) {
        if (current == internal) {
            airy_mtx_unlock(&g_registry.registry_mutex);
            AIRY_LOG_DEBUG("Service '%s' already registered", internal->name);
            return AIRY_SUCCESS;
        }
    }

    internal->next = g_registry.services;
    g_registry.services = internal;
    g_registry.service_count++;

    airy_mtx_unlock(&g_registry.registry_mutex);

    AIRY_LOG_INFO("Service '%s' explicitly registered (total: %u)", internal->name,
             g_registry.service_count);
    return AIRY_SUCCESS;
}

airy_err_t airy_svc_unregister(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    if (!g_registry.initialized) {
        return AIRY_ENOTINIT;
    }

    airy_svc_internal_t *internal = (airy_svc_internal_t *)svc;
    airy_mtx_lock(&g_registry.registry_mutex);

    airy_svc_internal_t **prev = &g_registry.services;
    airy_svc_internal_t *current = g_registry.services;

    while (current) {
        if (current == internal) {
            *prev = current->next;
            g_registry.service_count--;

            airy_mtx_unlock(&g_registry.registry_mutex);
            AIRY_LOG_INFO("Service '%s' unregistered (remaining: %u)", internal->name,
                     g_registry.service_count);
            return AIRY_SUCCESS;
        }

        prev = &current->next;
        current = current->next;
    }

    airy_mtx_unlock(&g_registry.registry_mutex);
    AIRY_LOG_WARN("Service '%s' not found in registry for unregistration", internal->name);
    return AIRY_ENOENT;
}

airy_svc_t airy_svc_find(const char *name)
{
    if (!name || !g_registry.initialized) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_mtx_lock(&g_registry.registry_mutex);

    airy_svc_internal_t *service = find_service_internal(name);

    airy_mtx_unlock(&g_registry.registry_mutex);

    return (airy_svc_t)service;
}

uint32_t airy_svc_count(void)
{
    if (!g_registry.initialized) {
        return 0;
    }

    airy_mtx_lock(&g_registry.registry_mutex);
    uint32_t count = g_registry.service_count;
    airy_mtx_unlock(&g_registry.registry_mutex);

    return count;
}

void airy_svc_foreach(airy_svc_enum_fn callback, void *user_data)
{
    if (!callback || !g_registry.initialized) {
        return;
    }

    airy_mtx_lock(&g_registry.registry_mutex);

    airy_svc_internal_t *current = g_registry.services;
    while (current) {
        callback((airy_svc_t)current, user_data);
        current = current->next;
    }

    airy_mtx_unlock(&g_registry.registry_mutex);
}

void airy_svc_common_cleanup(void)
{
    airy_msrep_shutdown();
    monitor_shutdown();
    svc_common_module_cleanup();
}
