// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file main.c
 * @brief Gateway daemon main entry (daemon module conventions).
 *
 * Conventions followed:
 * - ARCHITECTURAL_PRINCIPLES.md E-3 resource determinism (paired management)
 * - ARCHITECTURAL_PRINCIPLES.md E-4 cross-platform consistency (platform.h)
 * - ARCHITECTURAL_PRINCIPLES.md E-5 semantic naming (SVC_LOG_*)
 * - ARCHITECTURAL_PRINCIPLES.md E-6 traceable errors (AIRY_ERR_*)
 */

#include "atomic_compat.h"
#include "daemon_bootstrap_sd.h"
#include "daemon_cupolas_bootstrap.h"

#include "daemon_heapstore_bootstrap.h"
#include "daemon_ipc_ops_bootstrap.h"
#include "gateway_service.h"
#include "gateway_business_handler.h"
#include "gateway_biz_internal.h"
#include "gateway_cap_registry.h"
#include "gateway_d_internal.h"
#include "logging.h"
#include "daemon_platform_ext.h"
#include "svc_common.h"
#include "svc_logger.h"
#include "error.h"
#include "airy_rt.h"

#include "gateway_protocol_router.h"
#include "gateway_mcp_server.h"
#include "gateway_openai_compat.h"
#include "gateway_a2a_handler.h"

#ifdef AIRY_HAS_PROTOCOLS
#include "a2a_v03_adapter.h"
#include "mcp_v1_adapter.h"
#include "openai_enterprise_adapter.h"
#include "unified_protocol.h"
#endif

#include <signal.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h> /* write() in async-signal-safe handler */
#endif

static gateway_service_t g_service = NULL;
static atomic_int g_running = 1;
static daemon_bootstrap_sd_t *g_bsd = NULL;
static gateway_business_ctx_t *g_biz_ctx = NULL;
static gateway_entry_ctx_t g_entry_ctx;
static gw_proto_router_t *g_proto_router = NULL;

/**
 * @brief L2 standard method <ns>.shutdown callback (02-l2-service-protocol.md
 *        §6.1)
 *
 * Consistent with the signal-handling path: atomically clear g_running; the
 * main loop exits gracefully within its 1s poll. Triggered via the callback
 * when gateway_business_handle receives a "shutdown" RPC.
 */
static void gw_rpc_shutdown(void *user_data)
{
    (void)user_data;
    atomic_store_explicit(&g_running, 0, memory_order_seq_cst);
}

/**
 * @brief Signal handler (async-signal-safe: only sets an atomic flag; the
 *        actual stop action is done by the main loop)
 *
 * Must not call lock/alloc/log inside a signal handler (airy_mtx_lock,
 * gateway_service_stop etc. are not async-signal-safe); otherwise a signal
 * received while the main loop holds a lock would deadlock.
 */
static void signal_handler(int sig)
{
    atomic_store_explicit(&g_running, 0, memory_order_seq_cst);
#ifndef _WIN32
    {
        static const char sig_msg[] =
            "[SIG] shutdown signal received, initiating graceful shutdown\n";
        ssize_t written = write(STDERR_FILENO, sig_msg, sizeof(sig_msg) - 1);
        (void)written; /* best-effort diagnostics inside a signal handler */
    }
#endif
}

#ifdef _WIN32
/**
 * @brief Windows console event handler
 */
static BOOL WINAPI console_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static void svc_log_toggle_handler(int sig)
{
    (void)sig;
    static int debug_mode = 0;
    debug_mode = !debug_mode;
    log_set_module_level("*", debug_mode ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
}

int main(int argc, char *argv[])
{
    gateway_service_config_t config;
    bool daemonize = false;

    airy_sock_init();

    /* 参数解析与守护化动作分离：fork 必须在任何线程创建之前执行。
     * airy_init()/daemon_cupolas_init_pep() 会创建线程；POSIX fork 只复制
     * 调用线程，若 fork 时其它线程正持有 malloc/stdio 内部锁，子进程会继承
     * 已锁定状态，在后续清理路径中永久阻塞（SIGTERM 无法退出）。 */
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

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, svc_log_toggle_handler);
#endif

    log_init(NULL);
    atexit(log_cleanup);

#ifndef _WIN32
    if (daemonize) {
        SVC_LOG_INFO("Gateway daemonized (pid=%ld)", (long)getpid());
    }
#endif

    /* WS-8 stage 4 (8.4.1): bring up the corekern core (mem/oom/task/ipc/
     * eventloop/persist) as the first link of the daemon boot chain, before
     * the daemon's own subsystems. gateway_d hosts the gateway library (no
     * separate gateway process), so this call also powers the gateway side.
     * airy_init() is idempotent; if it fails the daemon still runs on the
     * platform fallbacks (DSL degradation, non-fatal, badge=0). */
    {
        int core_ret = airy_init();
        if (core_ret == AIRY_SUCCESS) {
            SVC_LOG_INFO("corekern core initialized (gateway_d runs on corekern)");
        } else {
            SVC_LOG_WARN("corekern init failed (%d) - running degraded (badge=0)", core_ret);
        }
    }

    daemon_cupolas_init_pep("gateway_d");

    /* Publish the IPC/RPC/SD ops table to atoms call sites so they
     * dispatch without linking daemons symbols. gateway_d links no
     * atoms engine target, but the table is process-wide and idempotent;
     * init failure is non-fatal. */
    daemon_ipc_ops_init("gateway_d");

    daemon_heapstore_init("gateway_d");

    SVC_LOG_INFO("Gateway service starting...");

    airy_err_t err = gateway_service_create(&g_service, &config);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to create service (err=%d)", err);
        goto cleanup;
    }

    err = gateway_service_init(g_service);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to init service (err=%d)", err);
        goto cleanup_service;
    }

    /* namespace 独占性门禁：cap registry 每个命名空间须登记
     * 独占 daemon、FWD 转发目标与归属一致；冲突 fail-closed 拒启
     * （边界从约定升级为启动期断言）。 */
    if (gw_cap_ns_validate() != 0) {
        SVC_LOG_ERROR("cap registry namespace ownership check failed, "
                      "refusing to start gateway");
        goto cleanup_service;
    }

    g_biz_ctx = gateway_business_ctx_create();
    if (!g_biz_ctx) {
        SVC_LOG_ERROR("Failed to create business handler context");
        goto cleanup_service;
    }

    /* L2 standard method <ns>.shutdown (02-l2-service-protocol.md §6.1):
     * inject the callback so that after receiving a "shutdown" RPC, graceful
     * exit identical to signal handling is triggered (atomically clear
     * g_running; the main loop exits within its 1s poll). */
    gateway_business_ctx_set_shutdown_cb(g_biz_ctx, gw_rpc_shutdown, NULL);

    gw_acl_register_defaults();

    g_proto_router = gw_proto_router_create();
    if (!g_proto_router) {
        SVC_LOG_ERROR("Failed to create protocol router");
        gateway_business_ctx_destroy(g_biz_ctx);
        g_biz_ctx = NULL;
        goto cleanup_service;
    }
    if (gw_proto_router_init(g_proto_router) != 0) {
        SVC_LOG_ERROR("Failed to init protocol router");
        gw_proto_router_destroy(g_proto_router);
        g_proto_router = NULL;
        gateway_business_ctx_destroy(g_biz_ctx);
        g_biz_ctx = NULL;
        goto cleanup_service;
    }

    /* Phase 2: adapter wiring — MCP tools -> tool_d / OpenAI -> llm_d /
     * A2A -> sched_d (protocol translation is concentrated in the gateway;
     * daemons have zero protocol knowledge, D2) */
    {

        gw_mcp_server_t *mcp = gw_proto_router_get_mcp(g_proto_router);
        if (mcp) {
            /* 内置工具目录唯一权威在 tool_d（registry + list_tools 输出
             * input_schema），gateway 启动时拉取注册（M1-1a SSoT 收敛）。 */
            int t_failed = gw_biz_mcp_register_tools(mcp, g_biz_ctx);
            if (t_failed < 0) {
                SVC_LOG_WARN("Phase 2: MCP tool catalog unavailable (tool_d down?)");
            } else if (t_failed != 0) {
                SVC_LOG_WARN("Phase 2: %d builtin tool(s) failed to register", t_failed);
            }
            SVC_LOG_INFO("Phase 2: MCP adapter wired — tool catalog from tool_d");

#ifndef _WIN32
            gw_mcp_clients_setup(mcp);
#endif
        }

        /* OpenAI: chat/completions → llm_d.complete */
        gw_openai_compat_t *openai = gw_proto_router_get_openai(g_proto_router);
        if (openai) {
            gw_openai_compat_set_llm_call(openai, gw_biz_llm_complete, g_biz_ctx);
            /* OpenAI: embeddings → llm_d.embeddings（RAG/知识库生态接入点） */
            gw_openai_compat_set_embed_fn(openai, gw_biz_llm_embeddings, g_biz_ctx);
            SVC_LOG_INFO("Phase 2: OpenAI adapter wired — chat/completions + embeddings → llm_d");
        }

        gw_a2a_handler_t *a2a = gw_proto_router_get_a2a(g_proto_router);
        if (a2a) {
            static const char *a2a_task_types[] = {"coding",  "analysis", "summarize",
                                                   "general", "devops",   NULL};
            for (int i = 0; a2a_task_types[i]; i++) {
                gw_a2a_handler_register_task_type(a2a, a2a_task_types[i], gw_biz_sched_schedule,
                                                  g_biz_ctx);
            }
            SVC_LOG_INFO("Phase 2: A2A adapter wired — task → sched_d");
        }
    }

    g_entry_ctx.biz_ctx = g_biz_ctx;
    g_entry_ctx.router = g_proto_router;
    err = gateway_service_set_handler(g_service, gateway_protocol_entry, &g_entry_ctx);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to register protocol entry handler (err=%d)", err);
        gw_proto_router_destroy(g_proto_router);
        g_proto_router = NULL;
        gateway_business_ctx_destroy(g_biz_ctx);
        g_biz_ctx = NULL;
        goto cleanup_service;
    }
    SVC_LOG_INFO("Protocol entry handler registered (MCP/OpenAI/A2A/JSON-RPC)");

    /* Initialize UnifiedProtocol stack for multi-protocol support */
#ifdef AIRY_HAS_PROTOCOLS
    const protocol_adapter_t *mcp_adapter = mcp_v1_get_adapter();
    if (mcp_adapter) {
        if (mcp_adapter->init(mcp_adapter->context) == 0) {
            SVC_LOG_INFO("MCP v1.0 adapter initialized (version=%s, caps=0x%x)",
                         mcp_adapter->version ? mcp_adapter->version : "unknown",
                         mcp_adapter->capabilities ?
                             mcp_adapter->capabilities(mcp_adapter->context) :
                             0);
        } else {
            SVC_LOG_WARN("Failed to initialize MCP v1.0 adapter");
        }
    } else {
        SVC_LOG_WARN("MCP v1.0 adapter not available");
    }
#endif

    err = gateway_service_start(g_service);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to start service (err=%d)", err);
        goto cleanup_service;
    }

    /* 端点摘要与 SD/IPC 注册必须读 service 实际生效配置（bind 侧
     * fail-closed 收敛发生在 start 内部，本地 config 副本不反映改写） */
    const gateway_service_config_t *run_cfg = gateway_get_config(g_service);

    SVC_LOG_INFO("AgentRT Gateway Daemon started");
    SVC_LOG_INFO("  HTTP:     %s:%d %s", run_cfg->http.host, run_cfg->http.port,
                 run_cfg->http.enabled ? "[enabled]" : "[disabled]");
    SVC_LOG_INFO("  WebSocket: %s:%d %s", run_cfg->ws.host, run_cfg->ws.port,
                 run_cfg->ws.enabled ? "[enabled]" : "[disabled]");
    SVC_LOG_INFO("  Stdio:    %s", run_cfg->stdio.enabled ? "[enabled]" : "[disabled]");

    g_bsd = daemon_bootstrap_sd_start("gateway_d", "gateway", run_cfg->http.host,
                                      run_cfg->http.port, "gateway,core", 0);

    int loop_count = 0;
    const int HEALTH_CHECK_INTERVAL = 30;

    while (atomic_load_explicit(&g_running, memory_order_acquire)) {
        if (!gateway_service_is_running(g_service)) {
            SVC_LOG_WARN("Gateway service stopped unexpectedly");
            break;
        }

        airy_sleep_ms(1000);
        loop_count++;

        if (run_cfg->enable_metrics && (loop_count % HEALTH_CHECK_INTERVAL == 0)) {
            airy_svc_stats_t stats;
            if (gateway_service_get_stats(g_service, &stats) == AIRY_SUCCESS) {
                SVC_LOG_INFO("Health Check [interval=%ds] "
                             "| concurrent=%u | total_req=%llu "
                             "| errors=%llu | avg_time=%.1fms",
                             HEALTH_CHECK_INTERVAL, stats.current_concurrent,
                             (unsigned long long)stats.request_count,
                             (unsigned long long)stats.error_count, stats.avg_time_ms);
            } else {
                SVC_LOG_WARN("Health check failed: unable to retrieve service stats");
            }
        }
    }

    daemon_bootstrap_sd_stop(g_bsd);

    SVC_LOG_INFO("Gateway shutting down...");
    gateway_service_stop(g_service, false);

    /* Cleanup protocol stack */
#ifdef AIRY_HAS_PROTOCOLS
    {
        const protocol_adapter_t *mcp_adapter = mcp_v1_get_adapter();
        if (mcp_adapter && mcp_adapter->destroy) {
            mcp_adapter->destroy(mcp_adapter->context);
            SVC_LOG_INFO("MCP adapter destroyed");
        }
    }
#endif

cleanup_service:

#ifndef _WIN32
    gw_mcp_client_cleanup();
#endif
    if (g_proto_router) {
        gw_proto_router_destroy(g_proto_router);
        g_proto_router = NULL;
    }
    if (g_biz_ctx) {
        gateway_business_ctx_destroy(g_biz_ctx);
        g_biz_ctx = NULL;
    }
    gateway_service_destroy(g_service);
cleanup:
    airy_sock_cleanup();

    SVC_LOG_INFO("Gateway daemon stopped");
    daemon_ipc_ops_cleanup();
    daemon_heapstore_cleanup();
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
