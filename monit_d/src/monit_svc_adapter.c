// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 *
 * @file monit_svc_adapter.c
 * @brief 监控服务适配器：将监控服务适配到统一的AgentRT服务管理框架
 *
 * 使用 airy_svc_set/get_user_data 存取适配器上下文，
 * 避免类型强转导致的类型安全问题。
 */

#include "monitor_service.h"
#include "svc_common.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    void *monit_svc;
    monitor_config_t monit_cfg;
    airy_svc_config_t common_cfg;
    bool owns_service;
    bool running;
} monit_adapter_ctx_t;

static monit_adapter_ctx_t *monit_get_ctx(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    return (monit_adapter_ctx_t *)airy_svc_get_user_data(service);
}

static void monit_config_from_common(monitor_config_t *monit_cfg,
                                     const airy_svc_config_t *common_cfg)
{
    __builtin_memset(monit_cfg, 0, sizeof(monitor_config_t));
    monit_cfg->metrics_collection_interval_ms = 5000;
    monit_cfg->health_check_interval_ms =
        (common_cfg && common_cfg->timeout_ms > 0) ? common_cfg->timeout_ms : 10000;
    monit_cfg->log_flush_interval_ms = 1000;
    monit_cfg->alert_check_interval_ms = 5000;
    monit_cfg->log_file_path = AIRY_STRDUP("./logs/monitor.log");
    monit_cfg->metrics_storage_path = AIRY_STRDUP("./metrics");
    monit_cfg->enable_tracing = (common_cfg && common_cfg->enable_tracing);
    monit_cfg->enable_alerting = true;
}

static airy_err_t monit_adapter_init(airy_svc_t service, const airy_svc_config_t *config)
{
    if (!service)
        return AIRY_EINVAL;
    monit_adapter_ctx_t *ctx = monit_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    }

    if (!ctx->monit_svc) {
        monit_config_from_common(&ctx->monit_cfg, &ctx->common_cfg);
        int ret = monitor_service_create(&ctx->monit_cfg, (monitor_service_t **)&ctx->monit_svc);
        if (ret != 0 || !ctx->monit_svc) {
            SVC_LOG_ERROR("监控服务创建失败: %d", ret);
            return AIRY_ERR_UNKNOWN;
        }
        ctx->owns_service = true;
    }

    return AIRY_SUCCESS;
}

static airy_err_t monit_adapter_start(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    monit_adapter_ctx_t *ctx = monit_get_ctx(service);
    if (!ctx || !ctx->monit_svc)
        return AIRY_ENOTINIT;
    if (ctx->running)
        return AIRY_SUCCESS;
    ctx->running = true;
    SVC_LOG_INFO("监控服务适配器已启动");
    return AIRY_SUCCESS;
}

static airy_err_t monit_adapter_stop(airy_svc_t service, bool force)
{
    if (!service)
        return AIRY_EINVAL;
    monit_adapter_ctx_t *ctx = monit_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->running)
        return AIRY_SUCCESS;
    ctx->running = false;
    if (force) {
        if (ctx->monit_svc && ctx->owns_service) {
            monitor_service_destroy(ctx->monit_svc);
            ctx->monit_svc = NULL;
            ctx->owns_service = false;
        }
        if (ctx->monit_cfg.log_file_path) {
            AIRY_FREE((void *)ctx->monit_cfg.log_file_path);
            ctx->monit_cfg.log_file_path = NULL;
        }
        if (ctx->monit_cfg.metrics_storage_path) {
            AIRY_FREE((void *)ctx->monit_cfg.metrics_storage_path);
            ctx->monit_cfg.metrics_storage_path = NULL;
        }
        SVC_LOG_INFO("监控服务适配器已强制停止");
    } else {
        SVC_LOG_INFO("监控服务适配器已停止");
    }
    return AIRY_SUCCESS;
}

static void monit_adapter_destroy(airy_svc_t service)
{
    if (!service)
        return;
    monit_adapter_ctx_t *ctx = monit_get_ctx(service);
    if (!ctx)
        return;

    if (ctx->monit_svc && ctx->owns_service) {
        monitor_service_destroy(ctx->monit_svc);
        ctx->monit_svc = NULL;
    }

    if (ctx->monit_cfg.log_file_path)
        AIRY_FREE((void *)ctx->monit_cfg.log_file_path);
    if (ctx->monit_cfg.metrics_storage_path)
        AIRY_FREE((void *)ctx->monit_cfg.metrics_storage_path);

    airy_svc_set_user_data(service, NULL);
    AIRY_FREE(ctx);
}

static airy_err_t monit_adapter_healthcheck(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    monit_adapter_ctx_t *ctx = monit_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;

    if (!ctx->monit_svc)
        return AIRY_ENOTINIT;

    health_check_result_t *result = NULL;
    int ret = monitor_service_health_check(ctx->monit_svc, "monitor_service", &result);

    if (ret != 0 || !result)
        return AIRY_ERR_UNKNOWN;

    airy_err_t err = result->is_healthy ? AIRY_SUCCESS : AIRY_ERR_UNKNOWN;

    AIRY_FREE(result->service_name);
    AIRY_FREE(result->status_message);
    AIRY_FREE(result);

    return err;
}

static const airy_svc_interface_t monit_adapter_iface = {
    .init = monit_adapter_init,
    .start = monit_adapter_start,
    .stop = monit_adapter_stop,
    .destroy = monit_adapter_destroy,
    .healthcheck = monit_adapter_healthcheck,
};

airy_err_t monit_service_adapter_create(airy_svc_t *out_service, const airy_svc_config_t *config)
{
    if (!out_service)
        return AIRY_EINVAL;

    monit_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(monit_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    } else {
        ctx->common_cfg.name = "monit_d";
        ctx->common_cfg.version = "0.1.1";
        ctx->common_cfg.enable_metrics = true;
        ctx->common_cfg.enable_tracing = true;
    }

    ctx->owns_service = true;

    airy_svc_t svc_handle = NULL;
    airy_err_t err =
        airy_svc_create(&svc_handle, ctx->common_cfg.name, &monit_adapter_iface, &ctx->common_cfg);
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

airy_err_t monit_service_adapter_wrap(airy_svc_t *out_service, void *monit_svc,
                                      const airy_svc_config_t *config)
{
    if (!out_service || !monit_svc)
        return AIRY_EINVAL;

    monit_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(monit_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    ctx->monit_svc = monit_svc;
    ctx->owns_service = false;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    } else {
        ctx->common_cfg.name = "monit_d";
        ctx->common_cfg.version = "0.1.1";
    }

    airy_svc_t svc_handle = NULL;
    airy_err_t err =
        airy_svc_create(&svc_handle, ctx->common_cfg.name, &monit_adapter_iface, &ctx->common_cfg);
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

void *monit_service_adapter_get_original(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    monit_adapter_ctx_t *ctx = monit_get_ctx(service);
    return ctx ? ctx->monit_svc : NULL;
}

airy_err_t monit_service_adapter_init(airy_svc_t service)
{
    return monit_adapter_init(service, NULL);
}

airy_err_t monit_service_adapter_start(airy_svc_t service)
{
    return monit_adapter_start(service);
}

airy_err_t monit_service_adapter_stop(airy_svc_t service, bool force)
{
    return monit_adapter_stop(service, force);
}

void monit_service_adapter_destroy(airy_svc_t service)
{
    monit_adapter_destroy(service);
}

airy_err_t monit_service_adapter_healthcheck(airy_svc_t service)
{
    return monit_adapter_healthcheck(service);
}

const airy_svc_interface_t *monit_service_adapter_get_interface(void)
{
    return &monit_adapter_iface;
}
