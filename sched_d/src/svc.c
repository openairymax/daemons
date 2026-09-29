/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief sched_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * 服务单例 + 生命周期四钩子 + attach 动态注册。策略装配集中在
 * svc_prepare：config 缺省、DAG 级联/并行环境覆盖、服务创建与蓝图
 * 调度器（roadmap.* 方法族）初始化。业务逻辑在域层（sched_service_*
 * / sched_dag_* / sched_rpc_handlers）；agent_d 真实派发链在
 * sched_dispatch.c，经 svc_activate 注入为服务 executor。
 */

#include "svc_sched_d.h"

#include "roadmap_rpc.h"
#include "sched_daemon_internal.h"
#include "svc_logger.h"

#include <stdlib.h>
#include <string.h>

/* 服务单例：机制层经钩子间接驱动，不直接访问 */
sched_service_t *g_service = NULL;

#define SCHED_CONFIG_DEFAULT "agentrt/manager/service/sched_d/sched.yaml"

static void sched_env_apply(sched_config_t *config)
{
    const char *fc = getenv("AIRY_DAG_FATAL_CASCADE");
    if (fc && *fc && strcmp(fc, "0") == 0) {
        config->dag_fatal_cascade = false;
        SVC_LOG_WARN("sched: DAG fatal-cascade disabled "
                     "(AIRY_DAG_FATAL_CASCADE=0) — any node failure aborts graph");
    }
    const char *par = getenv("AIRY_DAG_PARALLEL");
    if (par && *par) {
        unsigned long pv = strtoul(par, NULL, 10);
        if (pv > 0 && pv <= SCHED_DAG_MAX_NODES) {
            config->dag_max_parallel = (uint32_t)pv;
            SVC_LOG_INFO("sched: DAG parallel mode enabled via AIRY_DAG_PARALLEL=%lu", pv);
        } else {
            SVC_LOG_WARN("sched: invalid AIRY_DAG_PARALLEL=%s (1..%d), fallback serial",
                         par, SCHED_DAG_MAX_NODES);
        }
    }
}

int svc_prepare_sched_d(const char *config_path)
{
    if (!config_path || !*config_path)
        config_path = SCHED_CONFIG_DEFAULT;

    sched_config_t config = {.strategy = SCHED_STRATEGY_ROUND_ROBIN,
                             .health_check_interval_ms = 5000,
                             .stats_report_interval_ms = 10000,
                             .enable_ml_strategy = false,
                             .ml_model_path = NULL,
                             .max_agents = 100,
                             .dag_max_parallel = 0,
                             .dag_batch_size = 0,
                             .dag_fatal_cascade = true};
    sched_env_apply(&config);

    SVC_LOG_INFO("Scheduler service starting, manager=%s", config_path);
    int ret = sched_service_create(&config, &g_service);
    if (ret != AIRY_SUCCESS || !g_service) {
        SVC_LOG_ERROR("Failed to create scheduler service (error=%d)", ret);
        return -1;
    }

    if (roadmap_rpc_init() == AIRY_SUCCESS) {
        SVC_LOG_INFO("Roadmap scheduler (blueprint 3-tier) initialized");
    } else {
        SVC_LOG_WARN("Roadmap scheduler init failed - blueprint plan/absorb will be unavailable");
    }
    return 0;
}

void svc_endpoint_sched_d(daemon_endpoint_t *ep, int cmdline_tcp)
{
    ep->use_tcp = cmdline_tcp;
    ep->tcp_host = "127.0.0.1";
    ep->tcp_port = SCHED_D_TCP_PORT;
    ep->sock_unix = SCHED_D_SOCKET_UNIX;
    ep->sock_win = SCHED_D_SOCKET_WIN;
}

int svc_activate_sched_d(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd)
{
    (void)driver;
    (void)bsd;
    sched_service_set_executor(g_service, sched_dispatch_executor);
    if (sched_service_start_workers(g_service) != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to start scheduler worker thread");
        return -1;
    }
    return 0;
}

void svc_teardown_sched_d(void)
{
    /* worker 线程随 destroy 收束，无独立 teardown 策略 */
}

void svc_destroy_sched_d(void)
{
    roadmap_rpc_cleanup();
    if (g_service) {
        sched_service_destroy(g_service);
        g_service = NULL;
    }
}

/* 蓝图调度方法族（plan/absorb/cancel/replan/status）：注册逻辑内聚在
 * roadmap_rpc.c，此处仅透传 dispatcher */
void svc_attach_sched_d(void *dispatcher)
{
    roadmap_rpc_register(dispatcher);
}
