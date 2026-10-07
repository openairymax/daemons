// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file main.c
 * @brief Gateway daemon 装配骨架：装配序与主循环单点可见。
 *
 * 策略块（平台启动/信号/Phase 2 接线/协议栈/SD 公告/健康上报/全进程拆除）
 * 在 gw_boot.c。
 * Conventions: ARCHITECTURAL_PRINCIPLES.md E-3..E-6 (resource determinism,
 * cross-platform, SVC_LOG_*, AIRY_ERR_*).
 */

#include "atomic_compat.h"

#include "gateway_service.h"
#include "gateway_business_handler.h"
#include "gateway_cap_registry.h"
#include "gateway_d_internal.h"
#include "logging.h"
#include "daemon_platform_ext.h"
#include "svc_logger.h"
#include "error.h"

#include "gateway_protocol_router.h"

#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h> /* getpid() after daemonize */
#endif

static gateway_service_t g_service = NULL;
static atomic_int g_running = 1;
static gateway_business_ctx_t *g_biz_ctx = NULL;
static gateway_entry_ctx_t g_entry_ctx;
static gw_proto_router_t *g_proto_router = NULL;

int main(int argc, char *argv[])
{
    gateway_service_config_t config;
    bool daemonize = false;
    bool started = false;
    airy_err_t err;

    airy_sock_init();

    /* 解析与守护化分离：fork 先于一切线程创建（fork 只复制调用线程，
     * 锁继承会让子进程在清理路径永久阻塞）。 */
    if (gw_parse_args(argc, argv, &config, &daemonize) != 0) {
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

#ifndef _WIN32
    if (daemonize && gw_daemonize() != 0) {
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
#endif

    gw_sig_install(&g_running);

    log_init(NULL);
    atexit(log_cleanup);

#ifndef _WIN32
    if (daemonize) {
        SVC_LOG_INFO("Gateway daemonized (pid=%ld)", (long)getpid());
    }
#endif

    gw_plat_boot();

    SVC_LOG_INFO("Gateway service starting...");

    err = gateway_service_create(&g_service, &config);
    if (err == AIRY_SUCCESS)
        err = gateway_service_init(g_service);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("service create/init failed (err=%d)", err);
        goto fail;
    }

    /* namespace 独占性门禁：冲突 fail-closed 拒启。 */
    if (gw_cap_ns_validate() != 0) {
        SVC_LOG_ERROR("cap registry namespace ownership check failed, "
                      "refusing to start gateway");
        goto fail;
    }

    g_biz_ctx = gateway_business_ctx_create();
    if (!g_biz_ctx) {
        SVC_LOG_ERROR("Failed to create business handler context");
        goto fail;
    }

    gateway_business_ctx_set_shutdown_cb(g_biz_ctx, gw_rpc_stop, &g_running);

    gw_acl_register_defaults();

    g_proto_router = gw_proto_router_create();
    if (!g_proto_router) {
        SVC_LOG_ERROR("Failed to create protocol router");
        goto fail;
    }
    if (gw_proto_wire(g_proto_router, g_biz_ctx) != 0) {
        SVC_LOG_ERROR("Protocol router init/wiring failed");
        goto fail;
    }

    g_entry_ctx.biz_ctx = g_biz_ctx;
    g_entry_ctx.router = g_proto_router;
    err = gateway_service_set_handler(g_service, gateway_protocol_entry,
                                      &g_entry_ctx);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to register protocol entry handler (err=%d)", err);
        goto fail;
    }
    SVC_LOG_INFO("Protocol entry handler registered (MCP/OpenAI/A2A/JSON-RPC)");

    err = gateway_service_start(g_service);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to start service (err=%d)", err);
        goto fail;
    }
    started = true;

    gw_sd_announce(g_service);

    while (atomic_load_explicit(&g_running, memory_order_acquire)) {
        if (!gateway_service_is_running(g_service)) {
            SVC_LOG_WARN("Gateway service stopped unexpectedly");
            break;
        }

        airy_sleep_ms(1000);
        gw_health_tick(g_service);
    }

fail: /* early-start failures fall through with started=false */
    gw_teardown(g_service, g_proto_router, g_biz_ctx, started);
    return 0;
}
