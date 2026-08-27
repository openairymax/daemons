// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file mem_svc_adapter.c
 * @brief Adapts mem_service_t to the unified AgentRT service framework.
 *
 * Stores adapter context via airy_svc_set/get_user_data to avoid unsafe
 * type casts. Modeled on tool_svc_adapter.c.
 */

#include "mem_service.h"
#include "mem_svc_adapter.h"
#include "airyrt_version.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    mem_service_t *mem_svc;
    char *config_path;
    airy_svc_config_t common_cfg;
    bool owns_service;
    bool running;
} mem_adapter_ctx_t;

static mem_adapter_ctx_t *mem_get_ctx(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    return (mem_adapter_ctx_t *)airy_svc_get_user_data(service);
}

static int mem_adapter_init(airy_svc_t service, const airy_svc_config_t *config)
{
    if (!service)
        return AIRY_EINVAL;
    mem_adapter_ctx_t *ctx = mem_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;

    if (config) {
        __builtin_memset(&ctx->common_cfg, 0, sizeof(airy_svc_config_t));
        ctx->common_cfg.name = config->name ? AIRY_STRDUP(config->name) : NULL;
        ctx->common_cfg.version = config->version ? AIRY_STRDUP(config->version) : NULL;
        ctx->common_cfg.capabilities = config->capabilities;
        ctx->common_cfg.max_concurrent = config->max_concurrent;
        ctx->common_cfg.timeout_ms = config->timeout_ms;
        ctx->common_cfg.priority = config->priority;
        ctx->common_cfg.auto_start = config->auto_start;
        ctx->common_cfg.enable_metrics = config->enable_metrics;
        ctx->common_cfg.enable_tracing = config->enable_tracing;
    }

    if (!ctx->mem_svc) {
        ctx->mem_svc = mem_service_create(0);
        if (!ctx->mem_svc) {
            SVC_LOG_ERROR("Memory service creation failed");
            return AIRY_ERR_UNKNOWN;
        }
        ctx->owns_service = true;
    }

    return AIRY_SUCCESS;
}

static int mem_adapter_start(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    mem_adapter_ctx_t *ctx = mem_get_ctx(service);
    if (!ctx || !ctx->mem_svc)
        return AIRY_ENOTINIT;
    if (ctx->running)
        return AIRY_SUCCESS;
    ctx->running = true;
    SVC_LOG_INFO("Memory service adapter started");
    return AIRY_SUCCESS;
}

static int mem_adapter_stop(airy_svc_t service, bool force)
{
    if (!service)
        return AIRY_EINVAL;
    mem_adapter_ctx_t *ctx = mem_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->running)
        return AIRY_SUCCESS;
    ctx->running = false;
    if (force) {
        if (ctx->mem_svc && ctx->owns_service) {
            mem_service_destroy(ctx->mem_svc);
            ctx->mem_svc = NULL;
            ctx->owns_service = false;
        }
        if (ctx->config_path) {
            AIRY_FREE(ctx->config_path);
            ctx->config_path = NULL;
        }
        SVC_LOG_INFO("Memory service adapter force-stopped");
    } else {
        SVC_LOG_INFO("Memory service adapter stopped");
    }
    return AIRY_SUCCESS;
}

static void mem_adapter_destroy(airy_svc_t service)
{
    if (!service)
        return;
    mem_adapter_ctx_t *ctx = mem_get_ctx(service);
    if (!ctx)
        return;

    if (ctx->mem_svc && ctx->owns_service) {
        mem_service_destroy(ctx->mem_svc);
        ctx->mem_svc = NULL;
    }

    if (ctx->config_path) {
        AIRY_FREE(ctx->config_path);
        ctx->config_path = NULL;
    }

    if (ctx->common_cfg.name && ctx->common_cfg.name[0] != '\0') {
        AIRY_FREE((void *)ctx->common_cfg.name);
        ctx->common_cfg.name = NULL;
    }
    if (ctx->common_cfg.version && ctx->common_cfg.version[0] != '\0') {
        AIRY_FREE((void *)ctx->common_cfg.version);
        ctx->common_cfg.version = NULL;
    }

    airy_svc_set_user_data(service, NULL);
    AIRY_FREE(ctx);
}

static int mem_adapter_healthcheck(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    mem_adapter_ctx_t *ctx = mem_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->mem_svc)
        return AIRY_ENOTINIT;
    if (!ctx->running)
        return AIRY_ENOTINIT;

    size_t n = mem_service_count(ctx->mem_svc);
    SVC_LOG_INFO("Memory healthcheck: %zu records", n);
    return AIRY_SUCCESS;
}

static const airy_svc_interface_t mem_adapter_iface = {
    .init = mem_adapter_init,
    .start = mem_adapter_start,
    .stop = mem_adapter_stop,
    .destroy = mem_adapter_destroy,
    .healthcheck = mem_adapter_healthcheck,
};

int mem_service_adapter_create(airy_svc_t *out_service, const airy_svc_config_t *config)
{
    if (!out_service)
        return AIRY_EINVAL;

    mem_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(mem_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    if (config) {
        __builtin_memset(&ctx->common_cfg, 0, sizeof(airy_svc_config_t));
        ctx->common_cfg.name = config->name ? AIRY_STRDUP(config->name) : NULL;
        ctx->common_cfg.version = config->version ? AIRY_STRDUP(config->version) : NULL;
        ctx->common_cfg.capabilities = config->capabilities;
        ctx->common_cfg.max_concurrent = config->max_concurrent;
        ctx->common_cfg.timeout_ms = config->timeout_ms;
        ctx->common_cfg.priority = config->priority;
        ctx->common_cfg.auto_start = config->auto_start;
        ctx->common_cfg.enable_metrics = config->enable_metrics;
        ctx->common_cfg.enable_tracing = config->enable_tracing;
    } else {
        ctx->common_cfg.name = "mem_d";
        ctx->common_cfg.version = AIRYRT_VERSION;
        ctx->common_cfg.capabilities = AIRY_SVC_CAP_ASYNC;
        ctx->common_cfg.enable_metrics = true;
    }

    ctx->owns_service = true;

    airy_svc_t svc_handle = NULL;
    int err =
        airy_svc_create(&svc_handle, ctx->common_cfg.name, &mem_adapter_iface, &ctx->common_cfg);
    if (err != AIRY_SUCCESS) {
        AIRY_FREE(ctx);
        return err;
    }

    err = airy_svc_set_user_data(svc_handle, ctx);
    if (err != AIRY_SUCCESS) {
        airy_svc_destroy(svc_handle);
        AIRY_FREE(ctx);
        return err;
    }

    *out_service = svc_handle;
    return AIRY_SUCCESS;
}

int mem_service_adapter_wrap(airy_svc_t *out_service, mem_service_t *mem_svc,
                             const airy_svc_config_t *config)
{
    if (!out_service || !mem_svc)
        return AIRY_EINVAL;

    mem_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(mem_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    ctx->mem_svc = mem_svc;
    ctx->owns_service = false;

    if (config) {
        __builtin_memset(&ctx->common_cfg, 0, sizeof(airy_svc_config_t));
        ctx->common_cfg.name = config->name ? AIRY_STRDUP(config->name) : NULL;
        ctx->common_cfg.version = config->version ? AIRY_STRDUP(config->version) : NULL;
        ctx->common_cfg.capabilities = config->capabilities;
        ctx->common_cfg.max_concurrent = config->max_concurrent;
        ctx->common_cfg.timeout_ms = config->timeout_ms;
        ctx->common_cfg.priority = config->priority;
        ctx->common_cfg.auto_start = config->auto_start;
        ctx->common_cfg.enable_metrics = config->enable_metrics;
        ctx->common_cfg.enable_tracing = config->enable_tracing;
    } else {
        ctx->common_cfg.name = "mem_d";
        ctx->common_cfg.version = AIRYRT_VERSION;
    }

    airy_svc_t svc_handle = NULL;
    int err =
        airy_svc_create(&svc_handle, ctx->common_cfg.name, &mem_adapter_iface, &ctx->common_cfg);
    if (err != AIRY_SUCCESS) {
        AIRY_FREE(ctx);
        return err;
    }

    err = airy_svc_set_user_data(svc_handle, ctx);
    if (err != AIRY_SUCCESS) {
        airy_svc_destroy(svc_handle);
        AIRY_FREE(ctx);
        return err;
    }

    *out_service = svc_handle;
    return AIRY_SUCCESS;
}

mem_service_t *mem_service_adapter_get_original(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    mem_adapter_ctx_t *ctx = mem_get_ctx(service);
    return ctx ? ctx->mem_svc : NULL;
}

int mem_service_adapter_init(airy_svc_t service)
{
    return mem_adapter_init(service, NULL);
}

int mem_service_adapter_start(airy_svc_t service)
{
    return mem_adapter_start(service);
}

int mem_service_adapter_stop(airy_svc_t service, bool force)
{
    return mem_adapter_stop(service, force);
}

void mem_service_adapter_destroy(airy_svc_t service)
{
    mem_adapter_destroy(service);
}

int mem_service_adapter_healthcheck(airy_svc_t service)
{
    return mem_adapter_healthcheck(service);
}

const airy_svc_interface_t *mem_service_adapter_get_interface(void)
{
    return &mem_adapter_iface;
}
