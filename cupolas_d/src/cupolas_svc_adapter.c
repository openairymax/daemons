#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file cupolas_svc_adapter.c
 * @brief Cupolas 服务适配器：将 cupolas_service_t 适配到统一 AgentRT 服务管理框架
 *
 * 使用 airy_svc_set/get_user_data 存取适配器上下文，
 * 避免类型强转导致的类型安全问题。仿照 mem_svc_adapter.c 结构。
 */

#include "cupolas_svc_adapter.h"

#include "cupolas.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    cupolas_service_t *cupolas_svc;
    char *config_path;
    airy_svc_config_t common_cfg;
    bool owns_service;
    bool running;
} cupolas_adapter_ctx_t;

static cupolas_adapter_ctx_t *cupolas_get_ctx(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    return (cupolas_adapter_ctx_t *)airy_svc_get_user_data(service);
}

static int cupolas_adapter_init(airy_svc_t service, const airy_svc_config_t *config)
{
    if (!service)
        return AIRY_EINVAL;
    cupolas_adapter_ctx_t *ctx = cupolas_get_ctx(service);
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

    if (!ctx->cupolas_svc) {
        ctx->cupolas_svc = cupolas_service_create(ctx->config_path);
        if (!ctx->cupolas_svc) {
            SVC_LOG_ERROR("Cupolas service creation failed");
            return AIRY_ERR_UNKNOWN;
        }
        ctx->owns_service = true;
    }

    return AIRY_SUCCESS;
}

static int cupolas_adapter_start(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    cupolas_adapter_ctx_t *ctx = cupolas_get_ctx(service);
    if (!ctx || !ctx->cupolas_svc)
        return AIRY_ENOTINIT;
    if (ctx->running)
        return AIRY_SUCCESS;
    ctx->running = true;
    SVC_LOG_INFO("Cupolas service adapter started");
    return AIRY_SUCCESS;
}

static int cupolas_adapter_stop(airy_svc_t service, bool force)
{
    if (!service)
        return AIRY_EINVAL;
    cupolas_adapter_ctx_t *ctx = cupolas_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->running)
        return AIRY_SUCCESS;
    ctx->running = false;
    if (force) {
        if (ctx->cupolas_svc && ctx->owns_service) {
            cupolas_service_destroy(ctx->cupolas_svc);
            ctx->cupolas_svc = NULL;
            ctx->owns_service = false;
        }
        if (ctx->config_path) {
            AIRY_FREE(ctx->config_path);
            ctx->config_path = NULL;
        }
        SVC_LOG_INFO("Cupolas service adapter force-stopped");
    } else {
        SVC_LOG_INFO("Cupolas service adapter stopped");
    }
    return AIRY_SUCCESS;
}

static void cupolas_adapter_destroy(airy_svc_t service)
{
    if (!service)
        return;
    cupolas_adapter_ctx_t *ctx = cupolas_get_ctx(service);
    if (!ctx)
        return;

    if (ctx->cupolas_svc && ctx->owns_service) {
        cupolas_service_destroy(ctx->cupolas_svc);
        ctx->cupolas_svc = NULL;
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

static int cupolas_adapter_healthcheck(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    cupolas_adapter_ctx_t *ctx = cupolas_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->cupolas_svc)
        return AIRY_ENOTINIT;
    if (!ctx->running)
        return AIRY_ENOTINIT;
    /* 健康检查：服务实例存在且 cupolas 库版本可读（模块已初始化） */
    const char *version = cupolas_version();
    if (!version || version[0] == '\0') {
        SVC_LOG_WARN("Cupolas healthcheck failed: version unavailable");
        return AIRY_ERR_UNKNOWN;
    }
    SVC_LOG_INFO("Cupolas healthcheck: version=%s", version);
    return AIRY_SUCCESS;
}

static const airy_svc_interface_t cupolas_adapter_iface = {
    .init = cupolas_adapter_init,
    .start = cupolas_adapter_start,
    .stop = cupolas_adapter_stop,
    .destroy = cupolas_adapter_destroy,
    .healthcheck = cupolas_adapter_healthcheck,
};

int cupolas_service_adapter_create(airy_svc_t *out_service, const airy_svc_config_t *config)
{
    if (!out_service)
        return AIRY_EINVAL;

    cupolas_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(cupolas_adapter_ctx_t));
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
        if (config->name) {
            size_t path_len = strlen(config->name) + strlen("_config.json") + 1;
            ctx->config_path = AIRY_CALLOC(1, path_len);
            if (ctx->config_path) {
                snprintf(ctx->config_path, path_len, "%s_config.json", config->name);
            }
        }
    } else {
        ctx->common_cfg.name = "cupolas_d";
        ctx->common_cfg.version = "0.1.1";
        ctx->common_cfg.capabilities = AIRY_SVC_CAP_ASYNC;
        ctx->common_cfg.enable_metrics = true;
    }

    ctx->owns_service = true;

    airy_svc_t svc_handle = NULL;
    int err = airy_svc_create(&svc_handle, ctx->common_cfg.name, &cupolas_adapter_iface,
                              &ctx->common_cfg);
    if (err != AIRY_SUCCESS) {
        AIRY_FREE(ctx->config_path);
        AIRY_FREE(ctx);
        return err;
    }

    err = airy_svc_set_user_data(svc_handle, ctx);
    if (err != AIRY_SUCCESS) {
        airy_svc_destroy(svc_handle);
        AIRY_FREE(ctx->config_path);
        AIRY_FREE(ctx);
        return err;
    }

    *out_service = svc_handle;
    return AIRY_SUCCESS;
}

int cupolas_service_adapter_wrap(airy_svc_t *out_service, cupolas_service_t *cupolas_svc,
                                 const airy_svc_config_t *config)
{
    if (!out_service || !cupolas_svc)
        return AIRY_EINVAL;

    cupolas_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(cupolas_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    ctx->cupolas_svc = cupolas_svc;
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
        ctx->common_cfg.name = "cupolas_d";
        ctx->common_cfg.version = "0.1.1";
    }

    airy_svc_t svc_handle = NULL;
    int err = airy_svc_create(&svc_handle, ctx->common_cfg.name, &cupolas_adapter_iface,
                              &ctx->common_cfg);
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

cupolas_service_t *cupolas_service_adapter_get_original(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    cupolas_adapter_ctx_t *ctx = cupolas_get_ctx(service);
    return ctx ? ctx->cupolas_svc : NULL;
}

int cupolas_service_adapter_init(airy_svc_t service)
{
    return cupolas_adapter_init(service, NULL);
}

int cupolas_service_adapter_start(airy_svc_t service)
{
    return cupolas_adapter_start(service);
}

int cupolas_service_adapter_stop(airy_svc_t service, bool force)
{
    return cupolas_adapter_stop(service, force);
}

void cupolas_service_adapter_destroy(airy_svc_t service)
{
    cupolas_adapter_destroy(service);
}

int cupolas_service_adapter_healthcheck(airy_svc_t service)
{
    return cupolas_adapter_healthcheck(service);
}

const airy_svc_interface_t *cupolas_service_adapter_get_interface(void)
{
    return &cupolas_adapter_iface;
}
