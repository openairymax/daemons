#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file tool_svc_adapter.c
 * @brief 工具服务适配器：将工具服务适配到统一的AgentRT服务管理框架
 *
 * 使用 airy_svc_set/get_user_data 存取适配器上下文，
 * 避免类型强转导致的类型安全问题。
 */

#include "svc_common.h"
#include "svc_logger.h"
#include "tool_service.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    tool_service_t *tool_svc;
    char *config_path;
    airy_svc_config_t common_cfg;
    bool owns_service;
    bool running;
} tool_adapter_ctx_t;

static tool_adapter_ctx_t *tool_get_ctx(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    return (tool_adapter_ctx_t *)airy_svc_get_user_data(service);
}

static airy_err_t tool_adapter_init(airy_svc_t service,
                                         const airy_svc_config_t *config)
{
    if (!service)
        return AIRY_EINVAL;
    tool_adapter_ctx_t *ctx = tool_get_ctx(service);
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

    if (!ctx->tool_svc) {
        const char *path = ctx->config_path ? ctx->config_path : "tool_config.json";
        ctx->tool_svc = tool_service_create(path);
        if (!ctx->tool_svc) {
            SVC_LOG_ERROR("工具服务创建失败");
            return AIRY_ERR_UNKNOWN;
        }
        ctx->owns_service = true;
    }

    return AIRY_SUCCESS;
}

static airy_err_t tool_adapter_start(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    tool_adapter_ctx_t *ctx = tool_get_ctx(service);
    if (!ctx || !ctx->tool_svc)
        return AIRY_ENOTINIT;
    if (ctx->running)
        return AIRY_SUCCESS;
    ctx->running = true;
    SVC_LOG_INFO("工具服务适配器已启动");
    return AIRY_SUCCESS;
}

static airy_err_t tool_adapter_stop(airy_svc_t service, bool force)
{
    if (!service)
        return AIRY_EINVAL;
    tool_adapter_ctx_t *ctx = tool_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->running)
        return AIRY_SUCCESS;
    ctx->running = false;
    if (force) {
        if (ctx->tool_svc && ctx->owns_service) {
            tool_service_destroy(ctx->tool_svc);
            ctx->tool_svc = NULL;
            ctx->owns_service = false;
        }
        if (ctx->config_path) {
            AIRY_FREE(ctx->config_path);
            ctx->config_path = NULL;
        }
        SVC_LOG_INFO("工具服务适配器已强制停止");
    } else {
        SVC_LOG_INFO("工具服务适配器已停止");
    }
    return AIRY_SUCCESS;
}

static void tool_adapter_destroy(airy_svc_t service)
{
    if (!service)
        return;
    tool_adapter_ctx_t *ctx = tool_get_ctx(service);
    if (!ctx)
        return;

    if (ctx->tool_svc && ctx->owns_service) {
        tool_service_destroy(ctx->tool_svc);
        ctx->tool_svc = NULL;
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

static airy_err_t tool_adapter_healthcheck(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    tool_adapter_ctx_t *ctx = tool_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->tool_svc)
        return AIRY_ENOTINIT;
    if (!ctx->running)
        return AIRY_ENOTINIT;
    char *list_json = tool_service_list(ctx->tool_svc);
    if (!list_json) {
        SVC_LOG_WARN("工具服务健康检查失败: 无法获取工具列表");
        return AIRY_ERR_UNKNOWN;
    }
    AIRY_FREE(list_json);
    return AIRY_SUCCESS;
}

static const airy_svc_interface_t tool_adapter_iface = {
    .init = tool_adapter_init,
    .start = tool_adapter_start,
    .stop = tool_adapter_stop,
    .destroy = tool_adapter_destroy,
    .healthcheck = tool_adapter_healthcheck,
};

airy_err_t tool_service_adapter_create(airy_svc_t *out_service,
                                            const airy_svc_config_t *config)
{
    if (!out_service)
        return AIRY_EINVAL;

    tool_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(tool_adapter_ctx_t));
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
        ctx->common_cfg.name = "tool_d";
        ctx->common_cfg.version = "0.1.1";
        ctx->common_cfg.capabilities = AIRY_SVC_CAP_ASYNC;
        ctx->common_cfg.enable_metrics = true;
    }

    ctx->owns_service = true;

    airy_svc_t svc_handle = NULL;
    airy_err_t err = airy_svc_create(&svc_handle, ctx->common_cfg.name,
                                                 &tool_adapter_iface, &ctx->common_cfg);
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

airy_err_t tool_service_adapter_wrap(airy_svc_t *out_service, tool_service_t *tool_svc,
                                          const airy_svc_config_t *config)
{
    if (!out_service || !tool_svc)
        return AIRY_EINVAL;

    tool_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(tool_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    ctx->tool_svc = tool_svc;
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
        ctx->common_cfg.name = "tool_d";
        ctx->common_cfg.version = "0.1.1";
    }

    airy_svc_t svc_handle = NULL;
    airy_err_t err = airy_svc_create(&svc_handle, ctx->common_cfg.name,
                                                 &tool_adapter_iface, &ctx->common_cfg);
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

tool_service_t *tool_service_adapter_get_original(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    tool_adapter_ctx_t *ctx = tool_get_ctx(service);
    return ctx ? ctx->tool_svc : NULL;
}

airy_err_t tool_service_adapter_init(airy_svc_t service)
{
    return tool_adapter_init(service, NULL);
}

airy_err_t tool_service_adapter_start(airy_svc_t service)
{
    return tool_adapter_start(service);
}

airy_err_t tool_service_adapter_stop(airy_svc_t service, bool force)
{
    return tool_adapter_stop(service, force);
}

void tool_service_adapter_destroy(airy_svc_t service)
{
    tool_adapter_destroy(service);
}

airy_err_t tool_service_adapter_healthcheck(airy_svc_t service)
{
    return tool_adapter_healthcheck(service);
}

const airy_svc_interface_t *tool_service_adapter_get_interface(void)
{
    return &tool_adapter_iface;
}
