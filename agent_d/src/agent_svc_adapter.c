#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file agent_svc_adapter.c
 * @brief Agent 服务适配器：将 agent_service_t 适配到统一 AgentRT 服务管理框架
 *
 * 使用 airy_svc_set/get_user_data 存取适配器上下文，
 * 避免类型强转导致的类型安全问题。仿照 mem_svc_adapter.c 结构。
 */

#include "agent_service.h"
#include "agent_svc_adapter.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    agent_service_t *agent_svc;
    char *config_path;
    airy_svc_config_t common_cfg;
    bool owns_service;
    bool running;
} agent_adapter_ctx_t;

static agent_adapter_ctx_t *agent_get_ctx(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    return (agent_adapter_ctx_t *)airy_svc_get_user_data(service);
}

static int agent_adapter_init(airy_svc_t service, const airy_svc_config_t *config)
{
    if (!service)
        return AIRY_EINVAL;
    agent_adapter_ctx_t *ctx = agent_get_ctx(service);
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

    if (!ctx->agent_svc) {
        ctx->agent_svc = agent_service_create(0);
        if (!ctx->agent_svc) {
            SVC_LOG_ERROR("Agent service creation failed");
            return AIRY_ERR_UNKNOWN;
        }
        ctx->owns_service = true;
    }

    return AIRY_SUCCESS;
}

static int agent_adapter_start(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    agent_adapter_ctx_t *ctx = agent_get_ctx(service);
    if (!ctx || !ctx->agent_svc)
        return AIRY_ENOTINIT;
    if (ctx->running)
        return AIRY_SUCCESS;
    ctx->running = true;
    SVC_LOG_INFO("Agent service adapter started");
    return AIRY_SUCCESS;
}

static int agent_adapter_stop(airy_svc_t service, bool force)
{
    if (!service)
        return AIRY_EINVAL;
    agent_adapter_ctx_t *ctx = agent_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->running)
        return AIRY_SUCCESS;
    ctx->running = false;
    if (force) {
        if (ctx->agent_svc && ctx->owns_service) {
            agent_service_destroy(ctx->agent_svc);
            ctx->agent_svc = NULL;
            ctx->owns_service = false;
        }
        if (ctx->config_path) {
            AIRY_FREE(ctx->config_path);
            ctx->config_path = NULL;
        }
        SVC_LOG_INFO("Agent service adapter force-stopped");
    } else {
        SVC_LOG_INFO("Agent service adapter stopped");
    }
    return AIRY_SUCCESS;
}

static void agent_adapter_destroy(airy_svc_t service)
{
    if (!service)
        return;
    agent_adapter_ctx_t *ctx = agent_get_ctx(service);
    if (!ctx)
        return;

    if (ctx->agent_svc && ctx->owns_service) {
        agent_service_destroy(ctx->agent_svc);
        ctx->agent_svc = NULL;
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

static int agent_adapter_healthcheck(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    agent_adapter_ctx_t *ctx = agent_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->agent_svc)
        return AIRY_ENOTINIT;
    if (!ctx->running)
        return AIRY_ENOTINIT;
    /* 健康检查：服务存在且计数可读 */
    size_t n = agent_service_count(ctx->agent_svc);
    SVC_LOG_INFO("Agent healthcheck: %zu agents", n);
    return AIRY_SUCCESS;
}

static const airy_svc_interface_t agent_adapter_iface = {
    .init = agent_adapter_init,
    .start = agent_adapter_start,
    .stop = agent_adapter_stop,
    .destroy = agent_adapter_destroy,
    .healthcheck = agent_adapter_healthcheck,
};

int agent_service_adapter_create(airy_svc_t *out_service,
                                   const airy_svc_config_t *config)
{
    if (!out_service)
        return AIRY_EINVAL;

    agent_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(agent_adapter_ctx_t));
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
        ctx->common_cfg.name = "agent_d";
        ctx->common_cfg.version = "0.1.0";
        ctx->common_cfg.capabilities = AIRY_SVC_CAP_ASYNC;
        ctx->common_cfg.enable_metrics = true;
    }

    ctx->owns_service = true;

    airy_svc_t svc_handle = NULL;
    int err = airy_svc_create(&svc_handle, ctx->common_cfg.name, &agent_adapter_iface,
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

int agent_service_adapter_wrap(airy_svc_t *out_service, agent_service_t *agent_svc,
                                 const airy_svc_config_t *config)
{
    if (!out_service || !agent_svc)
        return AIRY_EINVAL;

    agent_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(agent_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    ctx->agent_svc = agent_svc;
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
        ctx->common_cfg.name = "agent_d";
        ctx->common_cfg.version = "0.1.0";
    }

    airy_svc_t svc_handle = NULL;
    int err = airy_svc_create(&svc_handle, ctx->common_cfg.name, &agent_adapter_iface,
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

agent_service_t *agent_service_adapter_get_original(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    agent_adapter_ctx_t *ctx = agent_get_ctx(service);
    return ctx ? ctx->agent_svc : NULL;
}

int agent_service_adapter_init(airy_svc_t service)
{
    return agent_adapter_init(service, NULL);
}

int agent_service_adapter_start(airy_svc_t service)
{
    return agent_adapter_start(service);
}

int agent_service_adapter_stop(airy_svc_t service, bool force)
{
    return agent_adapter_stop(service, force);
}

void agent_service_adapter_destroy(airy_svc_t service)
{
    agent_adapter_destroy(service);
}

int agent_service_adapter_healthcheck(airy_svc_t service)
{
    return agent_adapter_healthcheck(service);
}

const airy_svc_interface_t *agent_service_adapter_get_interface(void)
{
    return &agent_adapter_iface;
}
