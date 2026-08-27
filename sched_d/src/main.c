// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file main.c
 * @brief Scheduler service daemon main entry (daemon module conventions).
 *
 * Conventions followed:
 * - ARCHITECTURAL_PRINCIPLES.md E-3 resource determinism (paired management)
 * - ARCHITECTURAL_PRINCIPLES.md E-4 cross-platform consistency (platform.h)
 * - ARCHITECTURAL_PRINCIPLES.md E-5 semantic naming (SVC_LOG_*)
 * - ARCHITECTURAL_PRINCIPLES.md E-6 traceable errors (AIRY_ERR_*)
 *
 * Split by functional domain (single-responsibility): the JSON-RPC method
 * handlers live in sched_rpc_handlers.c and the agent_d real dispatch chain
 * in sched_dispatch.c; shared daemon symbols are declared in
 * sched_daemon_internal.h.
 */

#include "../../monit_d/include/monitor_service.h"
#include "daemon_main.h"
#include "daemon_rpc_client.h"
#include "platform.h"
#include "param_validator.h"
#include "roadmap_rpc.h"
#include "scheduler_service.h"
#include "sched_daemon_internal.h"
#include "strategy_interface.h"
#include "svc_logger.h"
#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("sched.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_sched"
#define DEFAULT_TCP_PORT 8083
#define MAX_BUFFER 65536

DAEMON_DECLARE_COMMON(sched_d, scheduler, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(sched_d)

sched_service_t *g_service = NULL;

#define SCHED_ERR_INVALID_PARAM AIRY_ERR_INVALID_PARAM
#define SCHED_ERR_OUT_OF_MEMORY AIRY_ERR_OUT_OF_MEMORY
#define SCHED_ERR_NOT_FOUND AIRY_ERR_NOT_FOUND
#define SCHED_ERR_INVALID_CONFIG (AIRY_ERR_DAEMON_BASE + 0x01)
#define SCHED_ERR_STRATEGY_FAIL (AIRY_ERR_DAEMON_BASE + 0x02)

static void destroy_service(void)
{
    roadmap_rpc_cleanup();
    if (g_service) {
        sched_service_destroy(g_service);
        g_service = NULL;
    }
}

int main(int argc, char **argv)
{
    const char *config_path = "agentrt/manager/service/sched_d/sched.yaml";
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_sched_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_sched_d);

#ifdef _WIN32
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler_sched_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(sched_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    daemon_cupolas_init("sched_d");

    SVC_LOG_INFO("Scheduler service starting, manager=%s", config_path);

    sched_config_t config = {.strategy = SCHED_STRATEGY_ROUND_ROBIN,
                             .health_check_interval_ms = 5000,
                             .stats_report_interval_ms = 10000,
                             .enable_ml_strategy = false,
                             .ml_model_path = NULL,
                             .max_agents = 100,
                             /* DAG parallel dispatch: 0 = serial (default,
                              * keeps legacy behavior). Env AIRY_DAG_PARALLEL=N
                              * (N>=1) enables mac_framework delegation-mode
                              * parallelism with N as the concurrency cap. */
                             .dag_max_parallel = 0,
                             .dag_batch_size = 0,
                             /* Failure-grading semantics (improvement 3):
                              * production defaults to only FATAL cascading
                              * graph cancellation; ordinary failures do not
                              * interrupt independent branches.
                              * AIRY_DAG_FATAL_CASCADE=0 restores legacy. */
                             .dag_fatal_cascade = true};
    {
        const char *dag_fc = getenv("AIRY_DAG_FATAL_CASCADE");
        if (dag_fc && dag_fc[0] != '\0' && strcmp(dag_fc, "0") == 0) {
            config.dag_fatal_cascade = false;
            SVC_LOG_WARN("sched: DAG fatal-cascade disabled "
                         "(AIRY_DAG_FATAL_CASCADE=0) — any node failure aborts graph");
        }
    }
    {
        const char *dag_par = getenv("AIRY_DAG_PARALLEL");
        if (dag_par && dag_par[0] != '\0') {
            unsigned long pv = strtoul(dag_par, NULL, 10);
            if (pv > 0 && pv <= SCHED_DAG_MAX_NODES) {
                config.dag_max_parallel = (uint32_t)pv;
                SVC_LOG_INFO("sched: DAG parallel mode enabled via AIRY_DAG_PARALLEL=%lu", pv);
            } else {
                SVC_LOG_WARN("sched: invalid AIRY_DAG_PARALLEL=%s (1..%d), fallback serial",
                             dag_par, SCHED_DAG_MAX_NODES);
            }
        }
    }

    int ret = sched_service_create(&config, &g_service);
    if (ret != AIRY_SUCCESS || !g_service) {
        SVC_LOG_ERROR("Failed to create scheduler service (error=%d)", ret);

        goto out_mtx_sock;
    }

    /* 蓝图调度接线（2026-08-25 修复）：创建 roadmap_sched 实例（L1 状态机 +
     * L2 语义缓存 + L3 全量规划三级路由），注册 roadmap.* RPC 方法族。 */
    if (roadmap_rpc_init() == AIRY_SUCCESS) {
        SVC_LOG_INFO("Roadmap scheduler (blueprint 3-tier) initialized");
    } else {
        SVC_LOG_WARN("Roadmap scheduler init failed - blueprint plan/absorb will be unavailable");
    }

    SVC_LOG_INFO("Scheduler service created with strategy: round_robin");

    airy_sock_t server_fd =
        daemon_create_server_socket(use_tcp, DEFAULT_TCP_PORT, DEFAULT_SOCKET_PATH_UNIX,
                                    DEFAULT_SOCKET_PATH_WIN);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");

        goto out_service;
    }
    SVC_LOG_INFO(use_tcp ? "Listening on TCP port %d" : "Listening on Unix socket",
                 DEFAULT_TCP_PORT);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = 4;
    ev_config.thread_pool_max = 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_sched_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = use_tcp ? "127.0.0.1" : DEFAULT_SOCKET_PATH_UNIX;
    ret =
        daemon_init_event_driver("sched_d", "scheduler", sock_addr, use_tcp ? DEFAULT_TCP_PORT : 0,
                                 "scheduler,core", use_tcp, &ev_config, &g_event_driver_sched_d,
                                 &g_bsd_sched_d, &g_bipc_sched_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_sched_d) {
        SVC_LOG_ERROR("Failed to create event driver");

        goto out_server_fd;
    }

    g_dispatcher_sched_d = daemon_event_driver_get_dispatcher(g_event_driver_sched_d);
    method_dispatcher_register(g_dispatcher_sched_d, "register_agent", on_register_agent_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "unregister_agent",
                               on_unregister_agent_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "schedule_task", on_schedule_task_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "get_task", on_get_task_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "cancel", on_cancel_task_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "dag_submit", on_dag_submit_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "dag_status", on_dag_status_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "dag_cancel", on_dag_cancel_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "get_stats", on_get_stats_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "health_check", on_health_check_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "checkpoint_save", on_checkpoint_save_method,
                               NULL);

    method_dispatcher_register(g_dispatcher_sched_d, "submit", on_schedule_task_method, NULL);
    method_dispatcher_register(g_dispatcher_sched_d, "query", on_get_task_method, NULL);

    /* 蓝图调度方法族（2026-08-25 接线）：plan（三级路由）/ absorb（蓝图注册
     * 与执行结果回灌）/ roadmap_cancel / roadmap_replan / roadmap_stats。 */
    roadmap_rpc_register(g_dispatcher_sched_d);

    method_dispatcher_register(g_dispatcher_sched_d, "shutdown", on_shutdown_method_sched_d, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (sched.* namespace)", 18);

    /* Inject the task execution callback and start the queue worker thread:
     * after schedule_task enqueues, the worker asynchronously completes
     * select agent -> spawn -> invoke (real dispatch); get_task queries status */
    sched_service_set_executor(g_service, sched_dispatch_executor);
    if (sched_service_start_workers(g_service) != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to start scheduler worker thread");
        goto out_event_driver;
    }

    if (daemon_event_driver_add_server_fd(g_event_driver_sched_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");

        goto out_event_driver;
    }

    SVC_LOG_INFO("Scheduler service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_sched_d);

    daemon_cleanup_standard(g_bipc_sched_d, g_bsd_sched_d, g_event_driver_sched_d, server_fd,
                            DEFAULT_SOCKET_PATH_UNIX, destroy_service, &g_running_lock_sched_d);

    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;

out_event_driver:
    daemon_event_driver_destroy(g_event_driver_sched_d);
out_server_fd:
    airy_sock_close(server_fd);
out_service:
    destroy_service();
out_mtx_sock:
    airy_mtx_destroy(&g_running_lock_sched_d);
    airy_sock_cleanup();
    return EXIT_FAILURE;
}
