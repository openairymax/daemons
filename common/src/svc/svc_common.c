// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_common.c
 * @brief Common service implementation - service lifecycle domain (unified
 *        service-management framework).
 *
 * Implements service lifecycle transitions defined in svc_common.h:
 * create/destroy and the init/start/stop state machine (including the
 * SIGALRM-guarded force-stop with ZOMBIE degradation).
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
 *
 * Phase 2.3a split: the in-process registry and query/async operations
 * were split out of this file:
 * - registry domain  -> svc_common_registry.c (g_registry + find/count/
 *   foreach/register/unregister + airy_svc_common_cleanup)
 * - query/async domain -> svc_common_ops.c (state accessors, stats,
 *   healthcheck, capability/state-string conversion, async dispatch)
 * Cross-file shared airy_svc_internal_t / registry helpers are declared
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
#include "daemon_platform_ext.h"
#include "safe_string_utils.h"
#include "svc_logger.h"

#ifndef _WIN32
/* SIGALRM/alarm/sigaction are POSIX-only; the Windows branch does not rely
 * on signal.h (the force-stop timeout degrades to a warning on Windows,
 * see airy_svc_stop). */
#include <signal.h>
#endif
#include <string.h>
#include <unistd.h>

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

    AIRY_MEMSET(&service->stats, 0, sizeof(airy_svc_stats_t));

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
            AIRY_MEMSET(&new_act, 0, sizeof(new_act));
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
