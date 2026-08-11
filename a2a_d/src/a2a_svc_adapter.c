// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 *
 * @file a2a_svc_adapter.c
 * @brief A2A 服务适配器：将 a2a_service_t 适配到统一 AgentRT 服务管理框架
 *
 * 使用 airy_svc_set/get_user_data 存取适配器上下文，
 * 避免类型强转导致的类型安全问题。仿照 mem_svc_adapter.c 结构。
 */

#include "a2a_service.h"
#include "a2a_svc_adapter.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    a2a_service_t *a2a_svc;
    char *config_path;
    airy_svc_config_t common_cfg;
    bool owns_service;
    bool running;
} a2a_adapter_ctx_t;

static a2a_adapter_ctx_t *a2a_get_ctx(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    return (a2a_adapter_ctx_t *)airy_svc_get_user_data(service);
}

static int a2a_adapter_init(airy_svc_t service, const airy_svc_config_t *config)
{
    if (!service)
        return AIRY_EINVAL;
    a2a_adapter_ctx_t *ctx = a2a_get_ctx(service);
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

    if (!ctx->a2a_svc) {
        ctx->a2a_svc = a2a_service_create(0, 0);
        if (!ctx->a2a_svc) {
            SVC_LOG_ERROR("A2A service creation failed");
            return AIRY_ERR_UNKNOWN;
        }
        ctx->owns_service = true;
    }

    return AIRY_SUCCESS;
}

static int a2a_adapter_start(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    a2a_adapter_ctx_t *ctx = a2a_get_ctx(service);
    if (!ctx || !ctx->a2a_svc)
        return AIRY_ENOTINIT;
    if (ctx->running)
        return AIRY_SUCCESS;
    ctx->running = true;
    SVC_LOG_INFO("A2A service adapter started");
    return AIRY_SUCCESS;
}

static int a2a_adapter_stop(airy_svc_t service, bool force)
{
    if (!service)
        return AIRY_EINVAL;
    a2a_adapter_ctx_t *ctx = a2a_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->running)
        return AIRY_SUCCESS;
    ctx->running = false;
    if (force) {
        if (ctx->a2a_svc && ctx->owns_service) {
            a2a_service_destroy(ctx->a2a_svc);
            ctx->a2a_svc = NULL;
            ctx->owns_service = false;
        }
        if (ctx->config_path) {
            AIRY_FREE(ctx->config_path);
            ctx->config_path = NULL;
        }
        SVC_LOG_INFO("A2A service adapter force-stopped");
    } else {
        SVC_LOG_INFO("A2A service adapter stopped");
    }
    return AIRY_SUCCESS;
}

static void a2a_adapter_destroy(airy_svc_t service)
{
    if (!service)
        return;
    a2a_adapter_ctx_t *ctx = a2a_get_ctx(service);
    if (!ctx)
        return;

    if (ctx->a2a_svc && ctx->owns_service) {
        a2a_service_destroy(ctx->a2a_svc);
        ctx->a2a_svc = NULL;
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

static int a2a_adapter_healthcheck(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    a2a_adapter_ctx_t *ctx = a2a_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->a2a_svc)
        return AIRY_ENOTINIT;
    if (!ctx->running)
        return AIRY_ENOTINIT;

    size_t n = a2a_service_count(ctx->a2a_svc);
    SVC_LOG_INFO("A2A healthcheck: %zu agents", n);
    return AIRY_SUCCESS;
}

static const airy_svc_interface_t a2a_adapter_iface = {
    .init = a2a_adapter_init,
    .start = a2a_adapter_start,
    .stop = a2a_adapter_stop,
    .destroy = a2a_adapter_destroy,
    .healthcheck = a2a_adapter_healthcheck,
};

int a2a_service_adapter_create(airy_svc_t *out_service, const airy_svc_config_t *config)
{
    if (!out_service)
        return AIRY_EINVAL;

    a2a_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(a2a_adapter_ctx_t));
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
        ctx->common_cfg.name = "a2a_d";
        ctx->common_cfg.version = "0.1.1";
        ctx->common_cfg.capabilities = AIRY_SVC_CAP_ASYNC;
        ctx->common_cfg.enable_metrics = true;
    }

    ctx->owns_service = true;

    airy_svc_t svc_handle = NULL;
    int err =
        airy_svc_create(&svc_handle, ctx->common_cfg.name, &a2a_adapter_iface, &ctx->common_cfg);
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

int a2a_service_adapter_wrap(airy_svc_t *out_service, a2a_service_t *a2a_svc,
                             const airy_svc_config_t *config)
{
    if (!out_service || !a2a_svc)
        return AIRY_EINVAL;

    a2a_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(a2a_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    ctx->a2a_svc = a2a_svc;
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
        ctx->common_cfg.name = "a2a_d";
        ctx->common_cfg.version = "0.1.1";
    }

    airy_svc_t svc_handle = NULL;
    int err =
        airy_svc_create(&svc_handle, ctx->common_cfg.name, &a2a_adapter_iface, &ctx->common_cfg);
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

a2a_service_t *a2a_service_adapter_get_original(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    a2a_adapter_ctx_t *ctx = a2a_get_ctx(service);
    return ctx ? ctx->a2a_svc : NULL;
}

int a2a_service_adapter_init(airy_svc_t service)
{
    return a2a_adapter_init(service, NULL);
}

int a2a_service_adapter_start(airy_svc_t service)
{
    return a2a_adapter_start(service);
}

int a2a_service_adapter_stop(airy_svc_t service, bool force)
{
    return a2a_adapter_stop(service, force);
}

void a2a_service_adapter_destroy(airy_svc_t service)
{
    a2a_adapter_destroy(service);
}

int a2a_service_adapter_healthcheck(airy_svc_t service)
{
    return a2a_adapter_healthcheck(service);
}

const airy_svc_interface_t *a2a_service_adapter_get_interface(void)
{
    return &a2a_adapter_iface;
}
