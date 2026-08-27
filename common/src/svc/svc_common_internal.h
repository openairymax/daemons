/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc_common_internal.h
 * @brief Internal shared definitions of the svc_common static library
 *        (not public API).
 *
 * After the P1.x modular split of svc_common.c into svc_registry.c /
 * svc_config.c / svc_monitor.c / svc_client.c, and the Phase 2.3a split of
 * the remaining lifecycle core into svc_common.c / svc_common_registry.c /
 * svc_common_ops.c, the following definitions are shared across multiple
 * source files:
 *
 * - airy_svc_internal_t: internal service-instance struct. Both
 *   svc_common.c (lifecycle/internal registry) and svc_client.c (local
 *   direct-connect client reading iface/user_data) need its fields, so it
 *   lives in an internal header instead of a single .c file.
 * - MAX_SERVICE_NAME_LEN / MAX_SERVICE_VERSION_LEN: fixed-size array deps
 *   in the struct, also used by airy_svc_create() in svc_common.c.
 * - monitor_shutdown(): the monitor domain (svc_monitor.c) owns the
 *   monitoring global state; called by airy_svc_common_cleanup() in
 *   svc_common.c to destroy the monitor mutex.
 *
 * This header is for svc_common static-library sources only; it must not
 * be used by other daemons modules.
 *
 * @see agentrt/daemons/common/src/svc_common.c
 * @see agentrt/daemons/common/src/svc_monitor.c
 * @see agentrt/daemons/common/src/svc_client.c
 */

#ifndef AIRY_RT_DAEMON_COMMON_SVC_COMMON_INTERNAL_H
#define AIRY_RT_DAEMON_COMMON_SVC_COMMON_INTERNAL_H

#include "svc_common.h"

#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SERVICE_NAME_LEN 64
#define MAX_SERVICE_VERSION_LEN 32

/**
 * @brief Service instance internal structure
 */
typedef struct airy_svc_internal {

    char name[MAX_SERVICE_NAME_LEN];
    char version[MAX_SERVICE_VERSION_LEN];

    airy_svc_state_t state;
    airy_mtx_t state_mutex;

    airy_svc_config_t config;
    uint32_t capabilities;

    airy_svc_stats_t stats;
    airy_mtx_t stats_mutex;

    airy_svc_interface_t iface;

    uint64_t last_healthcheck_time;
    int healthcheck_failures;

    void *user_data;

    void *thread_pool;
    pthread_t *threads;
    size_t thread_count;

    struct airy_svc_internal *next;
} airy_svc_internal_t;

/**
 * @brief Shut down the service monitor subsystem.
 * @note Implementation lives in svc_monitor.c (g_monitor state is private
 *       to the monitor domain), called by airy_svc_common_cleanup() in
 *       svc_common.c on the process-exit path.
 */
void monitor_shutdown(void);

/**
 * @brief Lazy-initialize the in-process service registry (mutex + memory
 *        stats reporter).
 * @note Implementation lives in svc_common_registry.c (g_registry state is
 *       private to the registry domain); called by airy_svc_create() in
 *       svc_common.c before registering a new service instance.
 */
airy_err_t svc_common_module_init(void);

/**
 * @brief Insert/remove a service instance into/from the internal registry
 *        (thread-safe, duplicate names rejected).
 * @note Implementation lives in svc_common_registry.c; used by the
 *       lifecycle domain (svc_common.c) during create/destroy.
 */
airy_err_t register_service_internal(airy_svc_internal_t *service);
airy_err_t unregister_service_internal(airy_svc_internal_t *service);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_COMMON_SVC_COMMON_INTERNAL_H */
