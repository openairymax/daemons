// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 *
 * @file gateway_svc_adapter.c
 * @brief Gateway服务适配器：将网关服务适配到统一的AgentRT服务管理框架
 *
 * 本文件实现了网关服务与airy_svc_t通用接口的适配层。
 * 通过适配器模式，网关服务可以无缝集成到服务管理框架中，
 * 享受统一的生命周期管理、状态监控、服务发现等功能。
 *
 * 适配器设计遵循架构原则K-2（接口契约化原则）：
 * 1. 提供标准化的服务接口
 * 2. 保持向后兼容性
 * 3. 最小化性能开销
 *
 */

#include "gateway_service.h"
#include "svc_common.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief 网关服务适配器上下文
 */
typedef struct {
    gateway_service_t gateway_svc;
    gateway_service_config_t gateway_cfg;
    airy_svc_config_t common_cfg;
} gateway_adapter_ctx_t;

/**
 * @brief 适配器初始化函数
 */
static airy_err_t gateway_adapter_init(airy_svc_t service, const airy_svc_config_t *config)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    if (!ctx) {
        SVC_LOG_ERROR("gateway_adapter_init: adapter context is NULL");
        return AIRY_EINVAL;
    }

    if (config) {
        AIRY_MEMCPY(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    }

    gateway_service_get_default_config(&ctx->gateway_cfg);

    ctx->gateway_cfg.name = ctx->common_cfg.name ? ctx->common_cfg.name : "gateway_d";
    ctx->gateway_cfg.version = ctx->common_cfg.version ? ctx->common_cfg.version : "0.1.1";

    if (ctx->common_cfg.capabilities & AIRY_SVC_CAP_ASYNC) {
    }

    if (ctx->common_cfg.capabilities & AIRY_SVC_CAP_STREAMING) {
        ctx->gateway_cfg.ws.enabled = true;
    }

    if (ctx->common_cfg.capabilities & AIRY_SVC_CAP_TIMEOUT) {
        // 使用配置的超时设置
        ctx->gateway_cfg.http.timeout_ms = ctx->common_cfg.timeout_ms;
        ctx->gateway_cfg.ws.timeout_ms = ctx->common_cfg.timeout_ms;
    }

    ctx->gateway_cfg.enable_metrics = ctx->common_cfg.enable_metrics;
    ctx->gateway_cfg.enable_tracing = ctx->common_cfg.enable_tracing;

    airy_err_t err = gateway_service_create(&ctx->gateway_svc, &ctx->gateway_cfg);

    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("网关服务创建失败: %d", err);
        return err;
    }

    err = gateway_service_init(ctx->gateway_svc);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("网关服务初始化失败: %d", err);
        gateway_service_destroy(ctx->gateway_svc);
        ctx->gateway_svc = NULL;
        return err;
    }

    return AIRY_SUCCESS;
}

/**
 * @brief 适配器启动函数
 */
static airy_err_t gateway_adapter_start(airy_svc_t service)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    if (!ctx) {
        return AIRY_EINVAL;
    }

    if (!ctx->gateway_svc) {
        SVC_LOG_WARN("gateway_adapter_start: gateway service not initialized");
        return AIRY_ENOTINIT;
    }

    airy_err_t err = gateway_service_start(ctx->gateway_svc);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("网关服务启动失败: %d", err);
        return err;
    }

    return AIRY_SUCCESS;
}

/**
 * @brief 适配器停止函数
 */
static airy_err_t gateway_adapter_stop(airy_svc_t service, bool force)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    if (!ctx) {
        return AIRY_EINVAL;
    }

    if (!ctx->gateway_svc) {
        SVC_LOG_WARN("gateway_adapter_stop: gateway service not initialized");
        return AIRY_ENOTINIT;
    }

    airy_err_t err = gateway_service_stop(ctx->gateway_svc, force);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("网关服务停止失败: %d", err);
        return err;
    }

    return AIRY_SUCCESS;
}

/**
 * @brief 适配器销毁函数
 */
static void gateway_adapter_destroy(airy_svc_t service)
{
    if (!service) {
        return;
    }

    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    if (!ctx) {
        return;
    }

    if (ctx->gateway_svc) {
        gateway_service_destroy(ctx->gateway_svc);
        ctx->gateway_svc = NULL;
    }

    if (ctx->gateway_cfg.http.host && strcmp(ctx->gateway_cfg.http.host, "0.0.0.0") != 0) {
        AIRY_FREE((void *)ctx->gateway_cfg.http.host);
    }
    if (ctx->gateway_cfg.ws.host && strcmp(ctx->gateway_cfg.ws.host, "0.0.0.0") != 0) {
        AIRY_FREE((void *)ctx->gateway_cfg.ws.host);
    }

    AIRY_FREE(ctx);

    airy_svc_set_user_data(service, NULL);
}

/**
 * @brief 适配器健康检查函数
 */
static airy_err_t gateway_adapter_healthcheck(airy_svc_t service)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    if (!ctx) {
        return AIRY_EINVAL;
    }

    if (!ctx->gateway_svc) {
        return AIRY_ENOTINIT;
    }

    return gateway_service_healthcheck(ctx->gateway_svc);
}

/**
 * @brief 网关服务适配器接口
 */
static const airy_svc_interface_t gateway_adapter_iface = {
    .init = gateway_adapter_init,
    .start = gateway_adapter_start,
    .stop = gateway_adapter_stop,
    .destroy = gateway_adapter_destroy,
    .healthcheck = gateway_adapter_healthcheck,
};

/**
 * @brief 创建网关服务适配器
 *
 * 此函数创建一个网关服务的适配器实例，使其能够通过统一的airy_svc_t接口进行管理。
 *
 * @param[out] out_service 输出服务句柄
 * @param[in] config 通用服务配置
 * @return 错误码
 */
airy_err_t gateway_service_adapter_create(airy_svc_t *out_service, const airy_svc_config_t *config)
{
    if (!out_service) {
        return AIRY_EINVAL;
    }

    gateway_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(gateway_adapter_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("gateway_service_adapter_create: context allocation failed");
        return AIRY_ENOMEM;
    }

    if (config) {
        AIRY_MEMCPY(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    } else {
        ctx->common_cfg.name = "gateway_d";
        ctx->common_cfg.version = "0.1.1";
        ctx->common_cfg.capabilities = AIRY_SVC_CAP_ASYNC | AIRY_SVC_CAP_STREAMING;
        ctx->common_cfg.max_concurrent = 1000;
        ctx->common_cfg.timeout_ms = 30000;
        ctx->common_cfg.priority = 0;
        ctx->common_cfg.auto_start = true;
        ctx->common_cfg.enable_metrics = true;
        ctx->common_cfg.enable_tracing = false;
    }

    airy_svc_t svc_handle = NULL;
    airy_err_t err = airy_svc_create(&svc_handle, ctx->common_cfg.name, &gateway_adapter_iface,
                                     &ctx->common_cfg);

    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("gateway_service_adapter_create: airy_svc_create failed, err=%d", err);
        AIRY_FREE(ctx);
        return err;
    }

    // 而不是将service句柄强转为适配器上下文（避免类型混淆）
    err = airy_svc_set_user_data(svc_handle, ctx);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("gateway_service_adapter_create: set_user_data failed, err=%d", err);
        airy_svc_destroy(svc_handle);
        AIRY_FREE(ctx);
        return err;
    }

    *out_service = svc_handle;
    return AIRY_SUCCESS;
}

/**
 * @brief 获取原始网关服务句柄
 *
 * 用于需要直接访问网关服务特定功能的场景。
 *
 * @param service 适配器服务句柄
 * @return 原始网关服务句柄，或NULL
 */
gateway_service_t gateway_service_adapter_get_original(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    return ctx ? ctx->gateway_svc : NULL;
}

/**
 * @brief 将现有网关服务包装为适配器
 *
 * 此函数将已存在的网关服务实例包装为适配器，使其能够集成到服务管理框架中。
 *
 * @param[out] out_service 输出服务句柄
 * @param[in] gateway_svc 原始网关服务句柄
 * @param[in] config 通用服务配置
 * @return 错误码
 */
airy_err_t gateway_service_adapter_wrap(airy_svc_t *out_service, gateway_service_t gateway_svc,
                                        const airy_svc_config_t *config)
{
    if (!out_service || !gateway_svc) {
        return AIRY_EINVAL;
    }

    gateway_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(gateway_adapter_ctx_t));
    if (!ctx) {
        return AIRY_ENOMEM;
    }

    ctx->gateway_svc = gateway_svc;

    if (config) {
        AIRY_MEMCPY(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    } else {
        ctx->common_cfg.name = "gateway_d";
        ctx->common_cfg.version = "0.1.1";
        ctx->common_cfg.capabilities = AIRY_SVC_CAP_ASYNC | AIRY_SVC_CAP_STREAMING;
        ctx->common_cfg.max_concurrent = 1000;
        ctx->common_cfg.timeout_ms = 30000;
        ctx->common_cfg.auto_start = true;
        ctx->common_cfg.enable_metrics = true;
    }

    airy_svc_t svc_handle = NULL;
    airy_err_t err = airy_svc_create(&svc_handle, ctx->common_cfg.name, &gateway_adapter_iface,
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

/**
 * @brief 适配器服务初始化（代理函数）
 */
airy_err_t gateway_service_adapter_init(airy_svc_t service)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    return gateway_adapter_init(service, NULL);
}

/**
 * @brief 适配器服务启动（代理函数）
 */
airy_err_t gateway_service_adapter_start(airy_svc_t service)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    return gateway_adapter_start(service);
}

/**
 * @brief 适配器服务停止（代理函数）
 */
airy_err_t gateway_service_adapter_stop(airy_svc_t service, bool force)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    return gateway_adapter_stop(service, force);
}

/**
 * @brief 适配器服务销毁（代理函数）
 */
void gateway_service_adapter_destroy(airy_svc_t service)
{
    gateway_adapter_destroy(service);
}

/**
 * @brief 适配器服务健康检查（代理函数）
 */
airy_err_t gateway_service_adapter_healthcheck(airy_svc_t service)
{
    if (!service) {
        return AIRY_EINVAL;
    }

    return gateway_adapter_healthcheck(service);
}

/**
 * @brief 获取适配器服务状态
 */
airy_svc_state_t gateway_service_adapter_get_state(airy_svc_t service)
{
    if (!service) {
        return AIRY_SVC_STATE_NONE;
    }

    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    if (!ctx || !ctx->gateway_svc) {
        return AIRY_SVC_STATE_NONE;
    }

    return gateway_service_get_state(ctx->gateway_svc);
}

/**
 * @brief 检查适配器服务是否运行中
 */
bool gateway_service_adapter_is_running(airy_svc_t service)
{
    if (!service) {
        return false;
    }

    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    if (!ctx || !ctx->gateway_svc) {
        return false;
    }

    return gateway_service_is_running(ctx->gateway_svc);
}

/**
 * @brief 获取适配器服务统计信息
 */
airy_err_t gateway_service_adapter_get_stats(airy_svc_t service, airy_svc_stats_t *stats)
{
    if (!service || !stats) {
        return AIRY_EINVAL;
    }

    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    if (!ctx || !ctx->gateway_svc) {
        return AIRY_ENOTINIT;
    }

    return gateway_service_get_stats(ctx->gateway_svc, stats);
}

const airy_svc_interface_t *gateway_service_adapter_get_interface(void)
{
    return &gateway_adapter_iface;
}

bool gateway_service_adapter_supports_type(airy_svc_t service, gateway_daemon_type_t type)
{
    if (!service)
        return false;
    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    if (!ctx)
        return false;
    switch (type) {
    case GATEWAY_DAEMON_TYPE_HTTP:
        return ctx->gateway_cfg.http.enabled;
    case GATEWAY_DAEMON_TYPE_WS:
        return ctx->gateway_cfg.ws.enabled;
    case GATEWAY_DAEMON_TYPE_STDIO:
        return ctx->gateway_cfg.stdio.enabled;
    default:
        return false;
    }
}

airy_err_t gateway_service_adapter_set_type_enabled(airy_svc_t service, gateway_daemon_type_t type,
                                                    bool enabled)
{
    if (!service)
        return AIRY_EINVAL;
    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    if (!ctx)
        return AIRY_EINVAL;
    airy_svc_state_t state = airy_svc_get_state(service);
    if (state == AIRY_SVC_STATE_RUNNING) {
        return AIRY_EBUSY;
    }
    switch (type) {
    case GATEWAY_DAEMON_TYPE_HTTP:
        ctx->gateway_cfg.http.enabled = enabled;
        break;
    case GATEWAY_DAEMON_TYPE_WS:
        ctx->gateway_cfg.ws.enabled = enabled;
        break;
    case GATEWAY_DAEMON_TYPE_STDIO:
        ctx->gateway_cfg.stdio.enabled = enabled;
        break;
    default:
        return AIRY_EINVAL;
    }
    return AIRY_SUCCESS;
}

airy_err_t gateway_service_adapter_create_from_config(airy_svc_t *out_service,
                                                      const char *config_path)
{
    if (!out_service || !config_path)
        return AIRY_EINVAL;
    airy_svc_config_t config;
    AIRY_MEMSET(&config, 0, sizeof(config));
    config.name = "gateway_d";
    config.version = "0.1.1";
    config.capabilities = AIRY_SVC_CAP_ASYNC | AIRY_SVC_CAP_STREAMING;
    config.max_concurrent = 1000;
    config.timeout_ms = 30000;
    config.auto_start = true;
    config.enable_metrics = true;
    airy_err_t err = gateway_service_adapter_create(out_service, &config);
    if (err != AIRY_SUCCESS)
        return err;
    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(*out_service);
    if (ctx) {
        gateway_service_get_default_config(&ctx->gateway_cfg);
    }
    return AIRY_SUCCESS;
}

airy_err_t gateway_service_adapter_reload_config(airy_svc_t service, const char *config_path)
{
    if (!service)
        return AIRY_EINVAL;
    gateway_adapter_ctx_t *ctx = (gateway_adapter_ctx_t *)airy_svc_get_user_data(service);
    if (!ctx)
        return AIRY_EINVAL;
    airy_svc_state_t state = airy_svc_get_state(service);
    if (state == AIRY_SVC_STATE_RUNNING) {
        return AIRY_EBUSY;
    }
    if (config_path) {
        gateway_service_get_default_config(&ctx->gateway_cfg);
        SVC_LOG_INFO("网关服务配置已从 %s 重新加载", config_path);
    }
    return AIRY_SUCCESS;
}

airy_err_t gateway_service_adapter_register(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    return airy_svc_register(service);
}

airy_err_t gateway_service_adapter_unregister(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    return airy_svc_unregister(service);
}