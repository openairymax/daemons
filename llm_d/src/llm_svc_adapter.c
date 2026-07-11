#include "memory_compat.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file llm_svc_adapter.c
 * @brief LLM服务适配器：将LLM服务适配到统一的AgentRT服务管理框架
 *
 * LLM服务的create接口接受config_path字符串而非配置结构体，
 * 本适配器将通用服务配置转换为LLM服务所需的配置路径格式。
 * 使用 airy_svc_set/get_user_data 存取适配器上下文，
 * 避免类型强转导致的类型安全问题。
 */

#include "llm_service.h"
#include "svc_common.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    llm_service_t *llm_svc;
    char *config_path;
    airy_svc_config_t common_cfg;
    bool owns_service;
    bool running;
} llm_adapter_ctx_t;

static llm_adapter_ctx_t *llm_get_ctx(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    return (llm_adapter_ctx_t *)airy_svc_get_user_data(service);
}

static airy_err_t llm_adapter_init(airy_svc_t service,
                                        const airy_svc_config_t *config)
{
    if (!service)
        return AIRY_EINVAL;

    llm_adapter_ctx_t *ctx = llm_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    }

    if (!ctx->llm_svc) {
        const char *path = ctx->config_path ? ctx->config_path : "llm_config.json";
        ctx->llm_svc = llm_service_create(path);
        if (!ctx->llm_svc) {
            SVC_LOG_ERROR("LLM服务创建失败");
            return AIRY_ERR_UNKNOWN;
        }
        ctx->owns_service = true;
    }

    return AIRY_SUCCESS;
}

static airy_err_t llm_adapter_start(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    llm_adapter_ctx_t *ctx = llm_get_ctx(service);
    if (!ctx || !ctx->llm_svc)
        return AIRY_ENOTINIT;
    if (ctx->running)
        return AIRY_SUCCESS;
    ctx->running = true;
    SVC_LOG_INFO("LLM服务适配器已启动");
    return AIRY_SUCCESS;
}

static airy_err_t llm_adapter_stop(airy_svc_t service, bool force)
{
    if (!service)
        return AIRY_EINVAL;
    llm_adapter_ctx_t *ctx = llm_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->running)
        return AIRY_SUCCESS;
    ctx->running = false;
    if (force) {
        if (ctx->llm_svc && ctx->owns_service) {
            llm_service_destroy(ctx->llm_svc);
            ctx->llm_svc = NULL;
            ctx->owns_service = false;
        }
        if (ctx->config_path) {
            AIRY_FREE(ctx->config_path);
            ctx->config_path = NULL;
        }
        SVC_LOG_INFO("LLM服务适配器已强制停止");
    } else {
        SVC_LOG_INFO("LLM服务适配器已停止");
    }
    return AIRY_SUCCESS;
}

static void llm_adapter_destroy(airy_svc_t service)
{
    if (!service)
        return;
    llm_adapter_ctx_t *ctx = llm_get_ctx(service);
    if (!ctx)
        return;

    if (ctx->llm_svc && ctx->owns_service) {
        llm_service_destroy(ctx->llm_svc);
        ctx->llm_svc = NULL;
    }

    if (ctx->config_path) {
        AIRY_FREE(ctx->config_path);
        ctx->config_path = NULL;
    }

    airy_svc_set_user_data(service, NULL);
    AIRY_FREE(ctx);
}

static airy_err_t llm_adapter_healthcheck(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    llm_adapter_ctx_t *ctx = llm_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->llm_svc)
        return AIRY_ENOTINIT;
    if (!ctx->running)
        return AIRY_ENOTINIT;
    char *stats_json = NULL;
    int ret = llm_service_stats(ctx->llm_svc, &stats_json);
    if (ret != 0) {
        SVC_LOG_WARN("LLM服务健康检查失败: %d", ret);
        return AIRY_ERR_UNKNOWN;
    }
    if (stats_json)
        AIRY_FREE(stats_json);
    return AIRY_SUCCESS;
}

static const airy_svc_interface_t llm_adapter_iface = {
    .init = llm_adapter_init,
    .start = llm_adapter_start,
    .stop = llm_adapter_stop,
    .destroy = llm_adapter_destroy,
    .healthcheck = llm_adapter_healthcheck,
};

airy_err_t llm_service_adapter_create(airy_svc_t *out_service,
                                           const airy_svc_config_t *config)
{
    if (!out_service)
        return AIRY_EINVAL;

    llm_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(llm_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
        if (config->name) {
            size_t path_len = strlen(config->name) + strlen("_config.json") + 1;
            ctx->config_path = AIRY_CALLOC(1, path_len);
            if (ctx->config_path) {
                snprintf(ctx->config_path, path_len, "%s_config.json", config->name);
            }
        }
    } else {
        ctx->common_cfg.name = "llm_d";
        ctx->common_cfg.version = "0.1.0";
        ctx->common_cfg.capabilities = AIRY_SVC_CAP_ASYNC;
        ctx->common_cfg.enable_metrics = true;
    }

    ctx->owns_service = true;

    airy_svc_t svc_handle = NULL;
    airy_err_t err = airy_svc_create(&svc_handle, ctx->common_cfg.name,
                                                 &llm_adapter_iface, &ctx->common_cfg);
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

airy_err_t llm_service_adapter_wrap(airy_svc_t *out_service, llm_service_t *llm_svc,
                                         const airy_svc_config_t *config)
{
    if (!out_service || !llm_svc)
        return AIRY_EINVAL;

    llm_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(llm_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    ctx->llm_svc = llm_svc;
    ctx->owns_service = false;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    } else {
        ctx->common_cfg.name = "llm_d";
        ctx->common_cfg.version = "0.1.0";
    }

    airy_svc_t svc_handle = NULL;
    airy_err_t err = airy_svc_create(&svc_handle, ctx->common_cfg.name,
                                                 &llm_adapter_iface, &ctx->common_cfg);
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

llm_service_t *llm_service_adapter_get_original(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    llm_adapter_ctx_t *ctx = llm_get_ctx(service);
    return ctx ? ctx->llm_svc : NULL;
}

airy_err_t llm_service_adapter_init(airy_svc_t service)
{
    return llm_adapter_init(service, NULL);
}

airy_err_t llm_service_adapter_start(airy_svc_t service)
{
    return llm_adapter_start(service);
}

airy_err_t llm_service_adapter_stop(airy_svc_t service, bool force)
{
    return llm_adapter_stop(service, force);
}

void llm_service_adapter_destroy(airy_svc_t service)
{
    llm_adapter_destroy(service);
}

airy_err_t llm_service_adapter_healthcheck(airy_svc_t service)
{
    return llm_adapter_healthcheck(service);
}

const airy_svc_interface_t *llm_service_adapter_get_interface(void)
{
    return &llm_adapter_iface;
}
