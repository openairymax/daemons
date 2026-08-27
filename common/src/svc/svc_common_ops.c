// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file svc_common_ops.c
 * @brief Common service implementation - state query / async request domain.
 *
 * Phase 2.3a split from svc_common.c: read-only state accessors, stats,
 * health checks, capability and state-string conversion, async request
 * dispatch via the service thread pool, and user-data accessors.
 *
 * These operations only read/mutate a single service instance (guarded by
 * its own state_mutex/stats_mutex); the global registry lives in
 * svc_common_registry.c and the lifecycle transitions in svc_common.c.
 *
 * The public API surface (svc_common.h) is unchanged by this split.
 *
 * @see agentrt/daemons/common/src/svc_common.c (lifecycle domain)
 * @see agentrt/daemons/common/src/svc_common_registry.c (registry domain)
 * @see agentrt/daemons/common/src/svc_common_internal.h
 */

#include "svc_common.h"
#include "daemon_errors.h"
#include "svc_common_internal.h"

#include "airy_memory.h"
#include "airyrt_version.h"
#include "svc_logger.h"
#include "thread_pool.h"

#include <string.h>
#include <strings.h>

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
    AIRY_MEMSET(&service->stats, 0, sizeof(airy_svc_stats_t));
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
