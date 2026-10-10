/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief monit_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * monitor 服务单例 + 生命周期五钩子。本户为常量户：端点由
 * svc_endpoint 从生成头常量回填，cmdline use_tcp 只升不降；服务参数
 * 不依赖 config 文件，svc_prepare 仅以 config_path 落启动日志。
 * observe/info 两个内建域按原语义非阻断降级（init 失败仅记日志，
 * 对应方法族不可用）。业务逻辑在 service 域与 monit_rpc.c，
 * 30s 指标自监控定时器在 svc_activate 挂载。
 */

#include "svc_monit_d.h"

#include "monit_d_internal.h"
#include "info_rpc.h"
#include "observe_rpc.h"
#include "prometheus_exporter.h"
#include "svc_logger.h"

#include <time.h>

#define METRICS_REPORT_INTERVAL_MS 30000

monitor_service_t *g_service = NULL;
uint64_t g_service_start_time = 0;

static void metrics_tick(airy_event_loop_t *loop, uint64_t timer_id, void *user_data)
{
    (void)loop;
    (void)timer_id;
    (void)user_data;

    uint64_t scrape_count = 0, scrape_errors = 0;
    prometheus_exporter_get_scrape_stats(&scrape_count, &scrape_errors);
    SVC_LOG_INFO("C-L10: Metrics report — scrapes=%llu errors=%llu",
                 (unsigned long long)scrape_count, (unsigned long long)scrape_errors);

    prometheus_gauge_set("airy_monit_scrape_count", (double)scrape_count);
    prometheus_gauge_set("airy_monit_scrape_errors", (double)scrape_errors);
}

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_base(ep, cmdline_tcp, MONIT_D_SOCKET_UNIX,
                   MONIT_D_SOCKET_WIN, MONIT_D_TCP_PORT);
}

int svc_prepare(const char *config_path)
{
    g_service_start_time = (uint64_t)time(NULL);

    SVC_LOG_INFO("Monitor service starting, manager=%s",
                 config_path ? config_path : "agentrt/manager/service/monit_d/monit.yaml");

    monitor_config_t config = {.metrics_collection_interval_ms = 5000,
                               .health_check_interval_ms = 10000,
                               .log_flush_interval_ms = 30000,
                               .alert_check_interval_ms = 5000,
                               .log_file_path = "monitor.log",
                               .metrics_storage_path = "metrics",
                               .enable_tracing = true,
                               .enable_alerting = true};

    int ret = monitor_service_create(&config, &g_service);
    if (ret != AIRY_SUCCESS || !g_service) {
        SVC_LOG_ERROR("Failed to create monitor service (error=%d)", ret);
        g_service = NULL;
        return -1;
    }
    SVC_LOG_INFO("Monitor service created successfully");

    if (prometheus_exporter_init("monit_d") == 0) {
        int metrics_ret = prometheus_exporter_register_required_metrics();
        if (metrics_ret != 0) {
            SVC_LOG_WARN("C-L10: Some required metrics failed to register (ret=%d)", metrics_ret);
        }
    } else {
        SVC_LOG_ERROR("C-L10: Failed to initialize Prometheus exporter");
    }

    /* observe/info 为 monit_d 内建模块：失败仅降级，不阻断启动。 */
    if (observe_rpc_init() != AIRY_SUCCESS) {
        SVC_LOG_ERROR("observe module init failed, observe_* methods unavailable");
    }
    if (info_rpc_init() != AIRY_SUCCESS) {
        SVC_LOG_ERROR("info module init failed, info_* methods unavailable");
    } else if (info_rpc_start() != AIRY_SUCCESS) {
        SVC_LOG_WARN("info collector thread not started, info history will be stale");
    }

    return 0;
}

int svc_activate(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd)
{
    (void)bsd;

    daemon_event_driver_add_timer(driver, METRICS_REPORT_INTERVAL_MS, metrics_tick, NULL);
    SVC_LOG_INFO("C-L10: Metrics report timer registered (30s interval)");
    return 0;
}

void svc_destroy(void)
{
    observe_rpc_cleanup();
    info_rpc_cleanup();
    prometheus_exporter_shutdown();
    if (g_service) {
        monitor_service_destroy(g_service);
        g_service = NULL;
    }
}

/* observe / info 方法族走动态注册（内建模块，不入 manifest
 * methods 静态表）；注册时点由生成态 main 在静态表落库后调用。 */
void svc_attach(void *dispatcher)
{
    observe_rpc_register(dispatcher);
    info_rpc_register(dispatcher);
}
