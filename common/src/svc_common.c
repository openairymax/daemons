// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_common.c
 * @brief Common service implementation - service lifecycle domain (unified
 *        service-management framework).
 *
 * Implements service lifecycle management, state monitoring, health checks
 * and statistics collection defined in svc_common.h (service lifecycle
 * domain).
 *
 * Design principles:
 * 1. Unified service interface definition (K-2 interface contracts)
 * 2. Explicit lifecycle management
 * 3. Standardized error handling (E-6 traceable errors)
 * 4. Thread-safe implementation (E-5 concurrency safety)
 *
 * P1.x modular split: this file keeps only the service lifecycle domain;
 * the other domains were split out:
 * - registry domain  -> svc_registry.c (cross-process registry client)
 * - config domain    -> svc_config.c (config file loading and watching)
 * - monitor domain   -> svc_monitor.c (service monitoring and degradation)
 * - client domain    -> svc_client.c (service communication client)
 * Cross-file shared airy_svc_internal_t / monitor_shutdown() are declared
 * in svc_common_internal.h (internal to this static lib, not public API).
 *
 * @see agentrt/daemons/common/include/svc_common.h
 * @see agentrt/daemons/common/src/svc_common_internal.h
 * @see Service_Management_Framework_Design.md
 */

#include "svc_common.h"
/* P0.17 phase 4: explicitly include daemon_errors.h for the DAEMON_E*
 * extension codes. The commons svc_common.h (resolved first via -I order)
 * does not include daemon_errors.h, so daemon sources must include it
 * explicitly to get the DAEMON_EINIT/ESTATE/EHEALTH aliases. */
#include "daemon_errors.h"
#include "svc_common_internal.h"

#include "airy_memory.h"
#include "airyrt_version.h"
#include "daemon_platform_ext.h"
#include "memory_stats_reporter.h"
#include "safe_string_utils.h"
#include "svc_logger.h"
#include "thread_pool.h"

#ifndef _WIN32
/* SIGALRM/alarm/sigaction are POSIX-only; the Windows branch does not rely
 * on signal.h (the force-stop timeout degrades to a warning on Windows,
 * see airy_svc_stop). */
#include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_SERVICES 256
#define DEFAULT_HEALTHCHECK_INTERVAL_MS 5000

/**
 * @brief Service registry internal state
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
static airy_err_t svc_common_module_init(void)
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
static airy_err_t register_service_internal(airy_svc_internal_t *service)
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
static airy_err_t unregister_service_internal(airy_svc_internal_t *service)
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

airy_err_t airy_svc_create(airy_svc_t *out_service, const char *name,
                           const airy_svc_interface_t *iface, const airy_svc_config_t *config)
{

    if (!out_service || !name || !iface || !config) {
        return AIRY_EINVAL;
    }

    airy_err_t err = svc_common_module_init();
    if (err != AIRY_SUCCESS) {
        return err;
    }

    size_t name_len = strlen(name);
    if (name_len == 0 || name_len >= MAX_SERVICE_NAME_LEN) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service =
        (airy_svc_internal_t *)AIRY_CALLOC(1, sizeof(airy_svc_internal_t));
    if (!service) {
        AIRY_ERROR(AIRY_ENOMEM, "airy_svc_create: calloc service failed");
    }

    if (safe_strcpy(service->name, name, MAX_SERVICE_NAME_LEN) != 0) {
        AIRY_FREE(service);
        AIRY_ERROR(AIRY_EINVAL, "airy_svc_create: name copy failed");
    }

    if (config->version) {
        if (safe_strcpy(service->version, config->version, MAX_SERVICE_VERSION_LEN) != 0) {
            AIRY_FREE(service);
            AIRY_ERROR(AIRY_EINVAL, "airy_svc_create: version copy failed");
        }
    }

    service->state = AIRY_SVC_STATE_CREATED;
    err = airy_mtx_init(&service->state_mutex);
    if (err != AIRY_SUCCESS) {
        AIRY_FREE(service);
        AIRY_ERROR(err, "airy_svc_create: state mutex init failed");
    }

    err = airy_mtx_init(&service->stats_mutex);
    if (err != AIRY_SUCCESS) {
        airy_mtx_destroy(&service->state_mutex);
        AIRY_FREE(service);
        return err;
    }

    __builtin_memcpy(&service->config, config, sizeof(airy_svc_config_t));
    service->capabilities = config->capabilities;

    __builtin_memcpy(&service->iface, iface, sizeof(airy_svc_interface_t));

    service->threads = NULL;
    service->thread_count = 0;

    __builtin_memset(&service->stats, 0, sizeof(airy_svc_stats_t));

    err = register_service_internal(service);
    if (err != AIRY_SUCCESS) {
        airy_mtx_destroy(&service->stats_mutex);
        airy_mtx_destroy(&service->state_mutex);
        AIRY_FREE(service);
        return err;
    }

    *out_service = (airy_svc_t)service;

    AIRY_LOG_INFO("Service '%s' created successfully", name);

    return AIRY_SUCCESS;
}

void airy_svc_destroy(airy_svc_t svc)
{
    if (!svc) {
        return;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    if (service->state == AIRY_SVC_STATE_RUNNING || service->state == AIRY_SVC_STATE_PAUSED) {
        airy_svc_stop((airy_svc_t)service, true);
    }

    {
        airy_err_t unreg_err = unregister_service_internal(service);
        if (unreg_err != AIRY_SUCCESS && unreg_err != AIRY_ENOENT) {
            AIRY_LOG_WARN("Service '%s' unregister during destroy returned %d - continuing cleanup",
                     service->name, unreg_err);
        }
    }

    if (service->iface.destroy) {
        service->iface.destroy(svc);
    }

    airy_mtx_destroy(&service->state_mutex);
    airy_mtx_destroy(&service->stats_mutex);

    if (service->threads) {
        AIRY_FREE(service->threads);
        service->threads = NULL;
        service->thread_count = 0;
    }
    AIRY_FREE(service);

    AIRY_LOG_INFO("Service destroyed");
}

airy_err_t airy_svc_init(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);

    if (service->state != AIRY_SVC_STATE_CREATED) {
        airy_mtx_unlock(&service->state_mutex);
        AIRY_LOG_ERROR("Service '%s' cannot initialize from state %d", service->name, service->state);
        return DAEMON_ESTATE;
    }

    service->state = AIRY_SVC_STATE_INITIALIZING;
    airy_mtx_unlock(&service->state_mutex);

    airy_err_t err = AIRY_SUCCESS;
    if (service->iface.init) {
        err = service->iface.init(svc, &service->config);
    }

    airy_mtx_lock(&service->state_mutex);
    if (err == AIRY_SUCCESS) {
        service->state = AIRY_SVC_STATE_READY;
        AIRY_LOG_INFO("Service '%s' initialized successfully", service->name);
    } else {
        service->state = AIRY_SVC_STATE_ERROR;
        AIRY_LOG_ERROR("Service '%s' initialization failed: %d", service->name, err);
    }

    airy_mtx_unlock(&service->state_mutex);

    return err;
}

airy_err_t airy_svc_start(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);

    if (service->state != AIRY_SVC_STATE_READY && service->state != AIRY_SVC_STATE_STOPPED &&
        service->state != AIRY_SVC_STATE_PAUSED && service->state != AIRY_SVC_STATE_ZOMBIE) {
        airy_mtx_unlock(&service->state_mutex);
        AIRY_LOG_ERROR("Service '%s' cannot start from state %d", service->name, service->state);
        return DAEMON_ESTATE;
    }

    airy_svc_state_t old_state = service->state;
    service->state = AIRY_SVC_STATE_RUNNING;
    airy_mtx_unlock(&service->state_mutex);

    airy_err_t err = AIRY_SUCCESS;
    if (service->iface.start) {
        err = service->iface.start(svc);
    }

    if (err != AIRY_SUCCESS) {
        airy_mtx_lock(&service->state_mutex);
        service->state = old_state;
        airy_mtx_unlock(&service->state_mutex);

        AIRY_LOG_ERROR("Service '%s' start failed: %d", service->name, err);
        return err;
    }

    AIRY_LOG_INFO("Service '%s' started successfully", service->name);

    return AIRY_SUCCESS;
}

#ifndef _WIN32
/* The force-stop timeout mechanism relies on SIGALRM/alarm, POSIX-only.
 * The Windows branch does not reference these symbols (see the _WIN32 branch
 * of airy_svc_stop). */
#define FORCE_STOP_TIMEOUT_SEC 5

static volatile sig_atomic_t g_svc_stop_timeout_flag = 0;

static void svc_stop_timeout_handler(int signum __attribute__((unused)))
{
    g_svc_stop_timeout_flag = 1;
}
#endif /* !_WIN32 */

airy_err_t airy_svc_stop(airy_svc_t svc, bool force)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);

    if (service->state != AIRY_SVC_STATE_RUNNING && service->state != AIRY_SVC_STATE_PAUSED) {
        airy_mtx_unlock(&service->state_mutex);
        AIRY_LOG_WARN("Service '%s' cannot stop from state %d", service->name, service->state);
        return DAEMON_ESTATE;
    }

    service->state = AIRY_SVC_STATE_STOPPING;
    airy_mtx_unlock(&service->state_mutex);

    airy_err_t err = AIRY_SUCCESS;
    bool zombie = false;

    if (service->iface.stop) {
#ifndef _WIN32
        struct sigaction old_act, new_act;
        if (force) {
            g_svc_stop_timeout_flag = 0;
            __builtin_memset(&new_act, 0, sizeof(new_act));
            new_act.sa_handler = svc_stop_timeout_handler;
            sigemptyset(&new_act.sa_mask);
            new_act.sa_flags = SA_RESETHAND;
            sigaction(SIGALRM, &new_act, &old_act);
            alarm(FORCE_STOP_TIMEOUT_SEC);
        }

        err = service->iface.stop(svc, force);

        if (force) {
            alarm(0);
            sigaction(SIGALRM, &old_act, NULL);
            if (g_svc_stop_timeout_flag) {
                zombie = true;
                AIRY_LOG_ERROR("Service '%s' force stop timed out after %d seconds - marking ZOMBIE",
                          service->name, FORCE_STOP_TIMEOUT_SEC);

#ifdef AIRY_OS_UNIX
                if (service->threads && service->thread_count > 0) {
                    AIRY_LOG_WARN("Service '%s' attempting deadlock recovery for %d threads",
                             service->name, (int)service->thread_count);
                    for (size_t i = 0; i < service->thread_count; i++) {
                        pthread_t tid = service->threads[i];
                        if (tid) {
                            void *retval = NULL;
                            int join_rc = pthread_tryjoin_np(tid, &retval);
                            if (join_rc == EBUSY) {
                                AIRY_LOG_ERROR("Service '%s' thread[%zu] deadlocked - cancelling",
                                          service->name, i);
                                pthread_cancel(tid);
                                pthread_join(tid, NULL);
                            } else if (join_rc == 0) {
                                AIRY_LOG_INFO("Service '%s' thread[%zu] joined with retval=%p",
                                         service->name, i, retval);
                            }
                        }
                    }
                }
#endif
            }
        }
#else /* _WIN32 */
        /* Windows has no SIGALRM/alarm/sigaction mechanism, so no timeout can
         * be imposed on force stop's hard kill. Degraded semantics: call the
         * service's stop directly; without timeout protection a blocking stop
         * hangs this call. Warn explicitly to avoid silent degradation
         * (ARCHITECTURAL_PRINCIPLES E-6 error traceability). The normal
         * (non-force) stop path is unaffected. */
        if (force) {
            AIRY_LOG_WARN("Service '%s' force stop timeout not supported on Windows "
                     "(SIGALRM/alarm unavailable) - force stop may hang",
                     service->name);
        }
        err = service->iface.stop(svc, force);
#endif /* _WIN32 */
    }

    airy_mtx_lock(&service->state_mutex);
    if (err == AIRY_SUCCESS || force) {
        service->state = zombie ? AIRY_SVC_STATE_ZOMBIE : AIRY_SVC_STATE_STOPPED;
        AIRY_LOG_INFO("Service '%s' stopped %s", service->name,
                 force ? (zombie ? "(ZOMBIE)" : "(forced)") : "gracefully");
    } else {
        service->state = AIRY_SVC_STATE_ERROR;
        AIRY_LOG_ERROR("Service '%s' stop failed: %d", service->name, err);
    }

    airy_mtx_unlock(&service->state_mutex);

    return err;
}

typedef struct {
    airy_svc_t service;
    char *method;
    char *params_json;
    airy_svc_async_complete_fn on_complete;
    void *user_data;
} async_request_context_t;

static void async_request_worker(void *arg)
{
    async_request_context_t *ctx = (async_request_context_t *)arg;
    if (!ctx)
        return;

    char *response_json = NULL;
    airy_err_t err = AIRY_EINVAL;

    airy_svc_internal_t *svc = (airy_svc_internal_t *)ctx->service;
    if (svc && svc->iface.handle_request) {
        err = svc->iface.handle_request(ctx->service, ctx->method, ctx->params_json, &response_json,
                                        svc->user_data);
    }

    if (ctx->on_complete) {
        ctx->on_complete(ctx->service, ctx->method, err, response_json, ctx->user_data);
    } else {
        if (response_json)
            AIRY_FREE(response_json);
    }

    AIRY_FREE(ctx->method);
    AIRY_FREE(ctx->params_json);
    AIRY_FREE(ctx);
}

airy_err_t airy_svc_set_thread_pool(airy_svc_t svc, void *pool)
{
    if (!svc)
        return AIRY_EINVAL;
    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;
    service->thread_pool = pool;
    return AIRY_SUCCESS;
}

int airy_svc_handle_request_async(airy_svc_t service, const char *method, const char *params_json,
                                  airy_svc_async_complete_fn on_complete, void *user_data)
{
    if (!service || !method) {
        AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "handle_request_async: null service or method");
    }

    airy_svc_internal_t *svc = (airy_svc_internal_t *)service;

    async_request_context_t *ctx = (async_request_context_t *)AIRY_CALLOC(1, sizeof(*ctx));
    if (!ctx) {
        AIRY_ERROR(AIRY_ERR_OUT_OF_MEMORY, "handle_request_async: calloc ctx failed");
    }

    ctx->service = service;
    ctx->method = AIRY_STRDUP(method);
    ctx->params_json = params_json ? AIRY_STRDUP(params_json) : NULL;
    ctx->on_complete = on_complete;
    ctx->user_data = user_data;

    if (svc->thread_pool) {
        int rc = thread_pool_submit(svc->thread_pool, async_request_worker, ctx);
        if (rc != 0) {
            AIRY_FREE(ctx->method);
            AIRY_FREE(ctx->params_json);
            AIRY_FREE(ctx);
            return rc;
        }
        return 0;
    }

    async_request_worker(ctx);
    return 0;
}

airy_err_t airy_svc_pause(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);

    if (service->state != AIRY_SVC_STATE_RUNNING) {
        airy_mtx_unlock(&service->state_mutex);
        AIRY_LOG_ERROR("Service '%s' cannot pause from state %d", service->name, service->state);
        return DAEMON_ESTATE;
    }

    if (!(service->capabilities & AIRY_SVC_CAP_PAUSEABLE)) {
        airy_mtx_unlock(&service->state_mutex);
        AIRY_LOG_ERROR("Service '%s' does not support pause", service->name);
        return AIRY_EPROTONOSUPPORT;
    }

    service->state = AIRY_SVC_STATE_PAUSED;
    airy_mtx_unlock(&service->state_mutex);

    AIRY_LOG_INFO("Service '%s' paused", service->name);

    return AIRY_SUCCESS;
}

airy_err_t airy_svc_resume(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);

    if (service->state != AIRY_SVC_STATE_PAUSED) {
        airy_mtx_unlock(&service->state_mutex);
        AIRY_LOG_ERROR("Service '%s' cannot resume from state %d", service->name, service->state);
        return DAEMON_ESTATE;
    }

    service->state = AIRY_SVC_STATE_RUNNING;
    airy_mtx_unlock(&service->state_mutex);

    AIRY_LOG_INFO("Service '%s' resumed", service->name);

    return AIRY_SUCCESS;
}

airy_svc_state_t airy_svc_get_state(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_SVC_STATE_NONE;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->state_mutex);
    airy_svc_state_t state = service->state;
    airy_mtx_unlock(&service->state_mutex);

    return state;
}

bool airy_svc_is_ready(airy_svc_t svc)
{
    airy_svc_state_t state = airy_svc_get_state(svc);
    return state == AIRY_SVC_STATE_READY;
}

bool airy_svc_is_running(airy_svc_t svc)
{
    airy_svc_state_t state = airy_svc_get_state(svc);
    return state == AIRY_SVC_STATE_RUNNING;
}

const char *airy_svc_get_name(airy_svc_t svc)
{
    if (!svc) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;
    return service->name;
}

const char *airy_svc_get_version(airy_svc_t svc)
{
    if (!svc) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;
    return service->version[0] ? service->version : AIRYRT_VERSION;
}

airy_err_t airy_svc_get_stats(airy_svc_t svc, airy_svc_stats_t *out_stats)
{

    if (!svc || !out_stats) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->stats_mutex);
    __builtin_memcpy(out_stats, &service->stats, sizeof(airy_svc_stats_t));
    airy_mtx_unlock(&service->stats_mutex);

    return AIRY_SUCCESS;
}

void airy_svc_reset_stats(airy_svc_t svc)
{
    if (!svc) {
        return;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    airy_mtx_lock(&service->stats_mutex);
    __builtin_memset(&service->stats, 0, sizeof(airy_svc_stats_t));
    airy_mtx_unlock(&service->stats_mutex);

    AIRY_LOG_DEBUG("Service '%s' stats reset", service->name);
}

airy_err_t airy_svc_healthcheck(airy_svc_t svc)
{
    if (!svc) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;

    if (service->iface.healthcheck) {
        airy_err_t err = service->iface.healthcheck(svc);

        uint64_t current_time = airy_time_ms();
        service->last_healthcheck_time = current_time;

        if (err != AIRY_SUCCESS) {
            service->healthcheck_failures++;
            AIRY_LOG_WARN("Service '%s' health check failed: %d (failures: %d)", service->name, err,
                     service->healthcheck_failures);
        } else {
            service->healthcheck_failures = 0;
        }

        return err;
    }

    airy_svc_state_t state = airy_svc_get_state(svc);

    switch (state) {
    case AIRY_SVC_STATE_READY:
    case AIRY_SVC_STATE_RUNNING:
    case AIRY_SVC_STATE_PAUSED:
        return AIRY_SUCCESS;

    case AIRY_SVC_STATE_ERROR:
        return DAEMON_EHEALTH;

    default:
        return DAEMON_ESTATE;
    }
}

bool airy_svc_has_capability(airy_svc_t svc, airy_svc_capability_t capability)
{

    if (!svc) {
        return false;
    }

    airy_svc_internal_t *service = (airy_svc_internal_t *)svc;
    return (service->capabilities & capability) != 0;
}

const char *airy_svc_state_to_string(airy_svc_state_t state)
{
    static const char *state_strings[] = {"NONE",   "CREATED",  "INITIALIZING", "READY",  "RUNNING",
                                          "PAUSED", "STOPPING", "STOPPED",      "ZOMBIE", "ERROR"};

    if (state < AIRY_SVC_STATE_NONE || state > AIRY_SVC_STATE_ERROR) {
        return "UNKNOWN";
    }

    return state_strings[state];
}

airy_svc_state_t airy_svc_state_from_string(const char *str)
{
    if (!str) {
        return AIRY_SVC_STATE_NONE;
    }

    static const struct {
        const char *name;
        airy_svc_state_t state;
    } state_map[] = {{"NONE", AIRY_SVC_STATE_NONE},
                     {"CREATED", AIRY_SVC_STATE_CREATED},
                     {"INITIALIZING", AIRY_SVC_STATE_INITIALIZING},
                     {"READY", AIRY_SVC_STATE_READY},
                     {"RUNNING", AIRY_SVC_STATE_RUNNING},
                     {"PAUSED", AIRY_SVC_STATE_PAUSED},
                     {"STOPPING", AIRY_SVC_STATE_STOPPING},
                     {"STOPPED", AIRY_SVC_STATE_STOPPED},
                     {"ZOMBIE", AIRY_SVC_STATE_ZOMBIE},
                     {"ERROR", AIRY_SVC_STATE_ERROR},
                     {NULL, AIRY_SVC_STATE_NONE}};

    for (int i = 0; state_map[i].name; i++) {
        if (strcasecmp(str, state_map[i].name) == 0) {
            return state_map[i].state;
        }
    }

    return AIRY_SVC_STATE_NONE;
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

airy_err_t airy_svc_set_user_data(airy_svc_t service, void *user_data)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    airy_svc_internal_t *internal = (airy_svc_internal_t *)service;
    airy_mtx_lock(&internal->state_mutex);
    internal->user_data = user_data;
    airy_mtx_unlock(&internal->state_mutex);

    return AIRY_SUCCESS;
}

void *airy_svc_get_user_data(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    airy_svc_internal_t *internal = (airy_svc_internal_t *)service;
    airy_mtx_lock(&internal->state_mutex);
    void *data = internal->user_data;
    airy_mtx_unlock(&internal->state_mutex);

    return data;
}

void airy_svc_common_cleanup(void)
{
    airy_msrep_shutdown();
    monitor_shutdown();
    svc_common_module_cleanup();
}
