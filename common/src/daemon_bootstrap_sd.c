// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file daemon_bootstrap_sd.c
 * @brief P1.7 C-L08: daemon ServiceDiscovery one-call bootstrap implementation.
 *
 * @see daemon_bootstrap_sd.h
 * @see P1.7 C-L08 wiring
 */

#include "daemon_bootstrap_sd.h"

#include "airy_memory.h"
#include "svc_logger.h"

#include <string.h>

struct daemon_bootstrap_sd_s {
    sd_helper_t *sdh;
    bool running;
    char service_name[64];
};

daemon_bootstrap_sd_t *daemon_bootstrap_sd_start(const char *name, const char *type,
                                                 const char *host, uint16_t port, const char *tags,
                                                 uint32_t ttl_ms)
{
    if (!name || !type || (!host && port == 0)) {
        SVC_LOG_ERROR("daemon_bootstrap_sd_start: invalid parameters");
        return NULL;
    }

    daemon_bootstrap_sd_t *bsd =
        (daemon_bootstrap_sd_t *)AIRY_CALLOC(1, sizeof(daemon_bootstrap_sd_t));
    if (!bsd) {
        SVC_LOG_ERROR("daemon_bootstrap_sd_start: OOM");
        return NULL;
    }

    bsd->sdh = sd_helper_init(NULL);
    if (!bsd->sdh) {
        SVC_LOG_ERROR("daemon_bootstrap_sd_start: sd_helper_init failed for '%s'", name);
        AIRY_FREE(bsd);
        return NULL;
    }

    if (host && port > 0) {
        if (sd_helper_register(bsd->sdh, name, type, host, port, tags, ttl_ms) != 0) {
            SVC_LOG_ERROR("daemon_bootstrap_sd_start: register failed for '%s' (%s:%u)", name, host,
                          port);
            sd_helper_shutdown(bsd->sdh);
            AIRY_FREE(bsd);
            return NULL;
        }
    } else {

        const char *sock = host ? host : name;
        if (sd_helper_register_unix(bsd->sdh, name, type, sock, tags, ttl_ms) != 0) {
            SVC_LOG_ERROR("daemon_bootstrap_sd_start: unix register failed for '%s'", name);
            sd_helper_shutdown(bsd->sdh);
            AIRY_FREE(bsd);
            return NULL;
        }
    }

    if (sd_helper_start_heartbeat(bsd->sdh) != 0) {
        SVC_LOG_WARN("daemon_bootstrap_sd_start: heartbeat start failed for '%s'", name);
    }

    AIRY_STRNCPY_TERM(bsd->service_name, name, sizeof(bsd->service_name) - 1);
    bsd->service_name[sizeof(bsd->service_name) - 1] = '\0';
    bsd->running = true;

    SVC_LOG_INFO("C-L08: ServiceDiscovery bootstrapped for '%s' (type=%s)", name, type);
    return bsd;
}

daemon_bootstrap_sd_t *daemon_bootstrap_sd_start_unix(const char *name, const char *type,
                                                      const char *socket_path, const char *tags,
                                                      uint32_t ttl_ms)
{
    if (!name || !type || !socket_path) {
        SVC_LOG_ERROR("daemon_bootstrap_sd_start_unix: invalid parameters");
        return NULL;
    }

    daemon_bootstrap_sd_t *bsd =
        (daemon_bootstrap_sd_t *)AIRY_CALLOC(1, sizeof(daemon_bootstrap_sd_t));
    if (!bsd)
        return NULL;

    bsd->sdh = sd_helper_init(NULL);
    if (!bsd->sdh) {
        SVC_LOG_ERROR("daemon_bootstrap_sd_start_unix: init failed for '%s'", name);
        AIRY_FREE(bsd);
        return NULL;
    }

    if (sd_helper_register_unix(bsd->sdh, name, type, socket_path, tags, ttl_ms) != 0) {
        SVC_LOG_ERROR("daemon_bootstrap_sd_start_unix: register failed for '%s'", name);
        sd_helper_shutdown(bsd->sdh);
        AIRY_FREE(bsd);
        return NULL;
    }

    if (sd_helper_start_heartbeat(bsd->sdh) != 0) {
        SVC_LOG_WARN("daemon_bootstrap_sd_start_unix: heartbeat start failed for '%s'", name);
    }

    AIRY_STRNCPY_TERM(bsd->service_name, name, sizeof(bsd->service_name) - 1);
    bsd->service_name[sizeof(bsd->service_name) - 1] = '\0';
    bsd->running = true;

    SVC_LOG_INFO("C-L08: ServiceDiscovery bootstrapped for '%s' (unix:%s)", name, socket_path);
    return bsd;
}

void daemon_bootstrap_sd_stop(daemon_bootstrap_sd_t *bsd)
{
    if (!bsd)
        return;

    SVC_LOG_INFO("C-L08: ServiceDiscovery shutting down for '%s'", bsd->service_name);

    if (bsd->sdh) {
        sd_helper_shutdown(bsd->sdh);
        bsd->sdh = NULL;
    }

    bsd->running = false;
    AIRY_FREE(bsd);
}

sd_helper_t *daemon_bootstrap_sd_get_helper(daemon_bootstrap_sd_t *bsd)
{
    return bsd ? bsd->sdh : NULL;
}

bool daemon_bootstrap_sd_is_running(daemon_bootstrap_sd_t *bsd)
{
    return bsd ? bsd->running : false;
}