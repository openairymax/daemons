/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief market_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * market 服务单例 + 生命周期五钩子。本户为常量户：存储路径不依赖
 * config 文件，storage_path=NULL 由服务层回落 $AIRY_HOME/agents（或
 * /skills），故 svc_prepare 忽略 config_path。端点由 svc_endpoint 从
 * 生成头常量回填，cmdline use_tcp 只升不降。业务逻辑在
 * market_service_*.c / publisher.c 与 market_rpc.c。
 */

#include "svc_market_d.h"

#include "market_d_internal.h"
#include "svc_logger.h"

market_service_t *g_service = NULL;

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_base(ep, cmdline_tcp, MARKET_D_SOCKET_UNIX,
                   MARKET_D_SOCKET_WIN, MARKET_D_TCP_PORT);
}

int svc_prepare(const char *config_path)
{
    (void)config_path;

    /* 常量户：NULL storage_path -> 服务层回落 $AIRY_HOME/agents | /skills，
     * 不依赖相对 CWD 的 "./agents"，避免随工作目录漂移。 */
    market_config_t config = {.registry_url = NULL,
                              .storage_path = NULL,
                              .sync_interval_ms = 30000,
                              .cache_ttl_ms = 3600000,
                              .enable_remote_registry = false,
                              .enable_auto_update = false};

    if (market_service_create(&config, &g_service) != AIRY_SUCCESS || !g_service) {
        SVC_LOG_ERROR("Failed to create market service");
        g_service = NULL;
        return -1;
    }
    SVC_LOG_INFO("market service started");
    return 0;
}

void svc_teardown(void)
{
}

void svc_destroy(void)
{
    if (g_service) {
        market_service_destroy(g_service);
        g_service = NULL;
    }
}

/* 无静态表外动态注册（manifest methods 全量覆盖），空实现 */
void svc_attach(void *dispatcher)
{
    (void)dispatcher;
}
