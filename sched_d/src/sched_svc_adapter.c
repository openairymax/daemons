#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file sched_svc_adapter.c
 * @brief 调度器服务适配器：将调度器服务适配到统一的AgentRT服务管理框架
 *
 * 使用 airy_svc_set/get_user_data 存取适配器上下文，
 * 避免类型强转导致的类型安全问题。
 */

#include "scheduler_service.h"
#include "svc_common.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    sched_service_t *sched_svc;
    sched_config_t sched_cfg;
    airy_svc_config_t common_cfg;
    bool owns_service;
    bool running;
} sched_adapter_ctx_t;

static sched_adapter_ctx_t *sched_get_ctx(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    return (sched_adapter_ctx_t *)airy_svc_get_user_data(service);
}

static void sched_config_from_common(sched_config_t *sched_cfg,
                                     const airy_svc_config_t *common_cfg)
{
    __builtin_memset(sched_cfg, 0, sizeof(sched_config_t));
    sched_cfg->strategy = SCHED_STRATEGY_WEIGHTED;
    sched_cfg->health_check_interval_ms =
        (common_cfg && common_cfg->timeout_ms > 0) ? common_cfg->timeout_ms : 10000;
    sched_cfg->stats_report_interval_ms = 60000;
    sched_cfg->enable_ml_strategy = (common_cfg && common_cfg->enable_metrics);
    sched_cfg->ml_model_path = NULL;
    sched_cfg->max_agents =
        (common_cfg && common_cfg->max_concurrent > 0) ? common_cfg->max_concurrent : 100;

    /* DAG 并行派发透传（语义与 main.c AIRY_DAG_PARALLEL 一致：0 = 串行）：
     * 服务声明 BATCH 能力（AIRY_SVC_CAP_BATCH）且 max_concurrent>0 时启用，
     * 并行度取 max_concurrent。clamp 至 SCHED_DAG_MAX_NODES——线程池按并行度
     * 创建常驻 worker（min=max），超限会创建失控数量的线程。
     * dag_batch_size 留 0：sched_service_create 默认取 dag_max_parallel。 */
    if (common_cfg && (common_cfg->capabilities & AIRY_SVC_CAP_BATCH) &&
        common_cfg->max_concurrent > 0) {
        sched_cfg->dag_max_parallel =
            (common_cfg->max_concurrent > SCHED_DAG_MAX_NODES)
                ? SCHED_DAG_MAX_NODES
                : common_cfg->max_concurrent;
    }
    /* 失败分级语义（改进3）：生产默认仅 FATAL 级联取消整图，
     * 普通失败不中断图其余独立分支。 */
    sched_cfg->dag_fatal_cascade = true;
}

static airy_err_t sched_adapter_init(airy_svc_t service,
                                          const airy_svc_config_t *config)
{
    if (!service)
        return AIRY_EINVAL;
    sched_adapter_ctx_t *ctx = sched_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    }

    if (!ctx->sched_svc) {
        sched_config_from_common(&ctx->sched_cfg, &ctx->common_cfg);
        int ret = sched_service_create(&ctx->sched_cfg, &ctx->sched_svc);
        if (ret != 0 || !ctx->sched_svc) {
            SVC_LOG_ERROR("调度器服务创建失败: %d", ret);
            return AIRY_ERR_UNKNOWN;
        }
        ctx->owns_service = true;
    }

    return AIRY_SUCCESS;
}

static airy_err_t sched_adapter_start(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    sched_adapter_ctx_t *ctx = sched_get_ctx(service);
    if (!ctx || !ctx->sched_svc)
        return AIRY_ENOTINIT;
    if (ctx->running)
        return AIRY_SUCCESS;
    ctx->running = true;
    SVC_LOG_INFO("调度器服务适配器已启动");
    return AIRY_SUCCESS;
}

static airy_err_t sched_adapter_stop(airy_svc_t service, bool force)
{
    if (!service)
        return AIRY_EINVAL;
    sched_adapter_ctx_t *ctx = sched_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->running)
        return AIRY_SUCCESS;
    ctx->running = false;
    if (force) {
        if (ctx->sched_svc && ctx->owns_service) {
            sched_service_destroy(ctx->sched_svc);
            ctx->sched_svc = NULL;
            ctx->owns_service = false;
        }
        if (ctx->sched_cfg.ml_model_path) {
            AIRY_FREE((void *)ctx->sched_cfg.ml_model_path);
            ctx->sched_cfg.ml_model_path = NULL;
        }
        SVC_LOG_INFO("调度器服务适配器已强制停止");
    } else {
        SVC_LOG_INFO("调度器服务适配器已停止");
    }
    return AIRY_SUCCESS;
}

static void sched_adapter_destroy(airy_svc_t service)
{
    if (!service)
        return;
    sched_adapter_ctx_t *ctx = sched_get_ctx(service);
    if (!ctx)
        return;

    if (ctx->sched_svc && ctx->owns_service) {
        sched_service_destroy(ctx->sched_svc);
        ctx->sched_svc = NULL;
    }

    if (ctx->sched_cfg.ml_model_path)
        AIRY_FREE((void *)ctx->sched_cfg.ml_model_path);

    airy_svc_set_user_data(service, NULL);
    AIRY_FREE(ctx);
}

static airy_err_t sched_adapter_healthcheck(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    sched_adapter_ctx_t *ctx = sched_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;

    if (!ctx->sched_svc)
        return AIRY_ENOTINIT;

    bool health_status = false;
    int ret = sched_service_health_check(ctx->sched_svc, &health_status);
    if (ret != 0 || !health_status)
        return AIRY_ERR_UNKNOWN;

    return AIRY_SUCCESS;
}

static const airy_svc_interface_t sched_adapter_iface = {
    .init = sched_adapter_init,
    .start = sched_adapter_start,
    .stop = sched_adapter_stop,
    .destroy = sched_adapter_destroy,
    .healthcheck = sched_adapter_healthcheck,
};

airy_err_t sched_service_adapter_create(airy_svc_t *out_service,
                                             const airy_svc_config_t *config)
{
    if (!out_service)
        return AIRY_EINVAL;

    sched_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(sched_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    } else {
        ctx->common_cfg.name = "sched_d";
        ctx->common_cfg.version = "0.1.1";
        ctx->common_cfg.enable_metrics = true;
    }

    ctx->owns_service = true;

    airy_svc_t svc_handle = NULL;
    airy_err_t err = airy_svc_create(&svc_handle, ctx->common_cfg.name,
                                                 &sched_adapter_iface, &ctx->common_cfg);
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

airy_err_t sched_service_adapter_wrap(airy_svc_t *out_service,
                                           sched_service_t *sched_svc,
                                           const airy_svc_config_t *config)
{
    if (!out_service || !sched_svc)
        return AIRY_EINVAL;

    sched_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(sched_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    ctx->sched_svc = sched_svc;
    ctx->owns_service = false;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    } else {
        ctx->common_cfg.name = "sched_d";
        ctx->common_cfg.version = "0.1.1";
    }

    airy_svc_t svc_handle = NULL;
    airy_err_t err = airy_svc_create(&svc_handle, ctx->common_cfg.name,
                                                 &sched_adapter_iface, &ctx->common_cfg);
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

sched_service_t *sched_service_adapter_get_original(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    sched_adapter_ctx_t *ctx = sched_get_ctx(service);
    return ctx ? ctx->sched_svc : NULL;
}

airy_err_t sched_service_adapter_init(airy_svc_t service)
{
    return sched_adapter_init(service, NULL);
}

airy_err_t sched_service_adapter_start(airy_svc_t service)
{
    return sched_adapter_start(service);
}

airy_err_t sched_service_adapter_stop(airy_svc_t service, bool force)
{
    return sched_adapter_stop(service, force);
}

void sched_service_adapter_destroy(airy_svc_t service)
{
    sched_adapter_destroy(service);
}

airy_err_t sched_service_adapter_healthcheck(airy_svc_t service)
{
    return sched_adapter_healthcheck(service);
}

const airy_svc_interface_t *sched_service_adapter_get_interface(void)
{
    return &sched_adapter_iface;
}
