// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file market_svc_adapter.c
 * @brief Adapts the market service to the unified AgentRT service framework.
 *
 * Stores adapter context via airy_svc_set/get_user_data to avoid unsafe
 * type casts.
 */

#include "market_service.h"
#include "svc_common.h"
#include "airyrt_version.h"
#include "svc_logger.h"
#include "platform.h" /* AIRY_HOME 权威路径：airy_data_dir() */

#include <stdlib.h>
#include <string.h>

typedef struct {
    market_service_t *market_svc;
    market_config_t market_cfg;
    airy_svc_config_t common_cfg;
    bool owns_service;
    bool running;
} market_adapter_ctx_t;

static market_adapter_ctx_t *market_get_ctx(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    return (market_adapter_ctx_t *)airy_svc_get_user_data(service);
}

static void market_config_from_common(market_config_t *market_cfg,
                                      const airy_svc_config_t *common_cfg)
{
    __builtin_memset(market_cfg, 0, sizeof(market_config_t));
    market_cfg->sync_interval_ms =
        (common_cfg && common_cfg->timeout_ms > 0) ? common_cfg->timeout_ms : 30000;
    market_cfg->cache_ttl_ms = 300000;
    market_cfg->enable_remote_registry = true;
    market_cfg->enable_auto_update = false;
    market_cfg->registry_url = AIRY_STRDUP("https://registry.agentrt.io");
    /* 收敛到 AIRY_HOME：市场数据 → $AIRY_HOME/data */
    {
        char path_buf[AIRY_PATH_MAX];
        snprintf(path_buf, sizeof(path_buf), "%s/market_data", airy_data_dir());
        market_cfg->storage_path = AIRY_STRDUP(path_buf);
    }
}

static airy_err_t market_adapter_init(airy_svc_t service, const airy_svc_config_t *config)
{
    if (!service)
        return AIRY_EINVAL;
    market_adapter_ctx_t *ctx = market_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    }

    if (!ctx->market_svc) {
        market_config_from_common(&ctx->market_cfg, &ctx->common_cfg);
        int ret = market_service_create(&ctx->market_cfg, &ctx->market_svc);
        if (ret != 0 || !ctx->market_svc) {
            SVC_LOG_ERROR("市场服务创建失败: %d", ret);
            return AIRY_ERR_UNKNOWN;
        }
        ctx->owns_service = true;
    }

    return AIRY_SUCCESS;
}

static airy_err_t market_adapter_start(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    market_adapter_ctx_t *ctx = market_get_ctx(service);
    if (!ctx || !ctx->market_svc)
        return AIRY_ENOTINIT;
    if (ctx->running)
        return AIRY_SUCCESS;
    ctx->running = true;
    SVC_LOG_INFO("市场服务适配器已启动");
    return AIRY_SUCCESS;
}

static airy_err_t market_adapter_stop(airy_svc_t service, bool force)
{
    if (!service)
        return AIRY_EINVAL;
    market_adapter_ctx_t *ctx = market_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->running)
        return AIRY_SUCCESS;
    ctx->running = false;
    if (force) {
        if (ctx->market_svc && ctx->owns_service) {
            market_service_destroy(ctx->market_svc);
            ctx->market_svc = NULL;
            ctx->owns_service = false;
        }
        if (ctx->market_cfg.registry_url) {
            AIRY_FREE((void *)ctx->market_cfg.registry_url);
            ctx->market_cfg.registry_url = NULL;
        }
        if (ctx->market_cfg.storage_path) {
            AIRY_FREE((void *)ctx->market_cfg.storage_path);
            ctx->market_cfg.storage_path = NULL;
        }
        SVC_LOG_INFO("市场服务适配器已强制停止");
    } else {
        SVC_LOG_INFO("市场服务适配器已停止");
    }
    return AIRY_SUCCESS;
}

static void market_adapter_destroy(airy_svc_t service)
{
    if (!service)
        return;
    market_adapter_ctx_t *ctx = market_get_ctx(service);
    if (!ctx)
        return;

    if (ctx->market_svc && ctx->owns_service) {
        market_service_destroy(ctx->market_svc);
        ctx->market_svc = NULL;
    }

    if (ctx->market_cfg.registry_url)
        AIRY_FREE((void *)ctx->market_cfg.registry_url);
    if (ctx->market_cfg.storage_path)
        AIRY_FREE((void *)ctx->market_cfg.storage_path);

    airy_svc_set_user_data(service, NULL);
    AIRY_FREE(ctx);
}

static airy_err_t market_adapter_healthcheck(airy_svc_t service)
{
    if (!service)
        return AIRY_EINVAL;
    market_adapter_ctx_t *ctx = market_get_ctx(service);
    if (!ctx)
        return AIRY_EINVAL;
    if (!ctx->market_svc)
        return AIRY_ENOTINIT;
    if (!ctx->running)
        return AIRY_ENOTINIT;
    agent_info_t **agents = NULL;
    size_t count = 0;
    search_params_t params = {0};
    int ret = market_service_search_agents(ctx->market_svc, &params, &agents, &count);
    if (ret != 0) {
        SVC_LOG_WARN("市场服务健康检查失败: %d", ret);
        return AIRY_ERR_UNKNOWN;
    }
    if (agents)
        AIRY_FREE(agents);
    return AIRY_SUCCESS;
}

static const airy_svc_interface_t market_adapter_iface = {
    .init = market_adapter_init,
    .start = market_adapter_start,
    .stop = market_adapter_stop,
    .destroy = market_adapter_destroy,
    .healthcheck = market_adapter_healthcheck,
};

airy_err_t market_service_adapter_create(airy_svc_t *out_service, const airy_svc_config_t *config)
{
    if (!out_service)
        return AIRY_EINVAL;

    market_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(market_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    } else {
        ctx->common_cfg.name = "market_d";
        ctx->common_cfg.version = AIRYRT_VERSION;
        ctx->common_cfg.enable_metrics = true;
    }

    ctx->owns_service = true;

    airy_svc_t svc_handle = NULL;
    airy_err_t err =
        airy_svc_create(&svc_handle, ctx->common_cfg.name, &market_adapter_iface, &ctx->common_cfg);
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

airy_err_t market_service_adapter_wrap(airy_svc_t *out_service, market_service_t *market_svc,
                                       const airy_svc_config_t *config)
{
    if (!out_service || !market_svc)
        return AIRY_EINVAL;

    market_adapter_ctx_t *ctx = AIRY_CALLOC(1, sizeof(market_adapter_ctx_t));
    if (!ctx)
        return AIRY_ENOMEM;

    ctx->market_svc = market_svc;
    ctx->owns_service = false;

    if (config) {
        __builtin_memcpy(&ctx->common_cfg, config, sizeof(airy_svc_config_t));
    } else {
        ctx->common_cfg.name = "market_d";
        ctx->common_cfg.version = AIRYRT_VERSION;
    }

    airy_svc_t svc_handle = NULL;
    airy_err_t err =
        airy_svc_create(&svc_handle, ctx->common_cfg.name, &market_adapter_iface, &ctx->common_cfg);
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

market_service_t *market_service_adapter_get_original(airy_svc_t service)
{
    if (!service) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    market_adapter_ctx_t *ctx = market_get_ctx(service);
    return ctx ? ctx->market_svc : NULL;
}

airy_err_t market_service_adapter_init(airy_svc_t service)
{
    return market_adapter_init(service, NULL);
}

airy_err_t market_service_adapter_start(airy_svc_t service)
{
    return market_adapter_start(service);
}

airy_err_t market_service_adapter_stop(airy_svc_t service, bool force)
{
    return market_adapter_stop(service, force);
}

void market_service_adapter_destroy(airy_svc_t service)
{
    market_adapter_destroy(service);
}

airy_err_t market_service_adapter_healthcheck(airy_svc_t service)
{
    return market_adapter_healthcheck(service);
}

const airy_svc_interface_t *market_service_adapter_get_interface(void)
{
    return &market_adapter_iface;
}
