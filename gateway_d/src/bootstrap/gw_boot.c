// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gw_boot.c
 * @brief gateway_d 启停策略域：平台启动、信号安装、协议装配、SD 公告、
 *        健康上报与全进程关停级联。
 *
 * main.c 只保留装配骨架与主循环；本域承接全部策略块——corekern 首启链接
 * 与 PEP/IPC/heapstore 平台服务发布、信号策略、Phase 2 适配器接线（MCP
 * 工具目录→tool_d / LLM API→llm_d / A2A→sched_d）、UnifiedProtocol 栈初始
 * 化、端点摘要与 SD 注册、周期健康上报、关停拆除（含 socket 面与 ops 逆序
 * 清理）。
 */

#include "airy_rt.h"
#include "atomic_compat.h"
#include "daemon_bootstrap_sd.h"
#include "daemon_heapstore_bootstrap.h"
#include "daemon_ipc_ops_bootstrap.h"
#include "daemon_security_dome.h"

#include "gateway_service.h"
#include "gateway_business_handler.h"
#include "gateway_biz_internal.h"
#include "gateway_d_internal.h"
#include "logging.h"
#include "daemon_platform_ext.h"
#include "platform_paths.h"
#include "svc_common.h"
#include "svc_logger.h"
#include "error.h"
#include "io.h"

#include "gateway_protocol_router.h"
#include "gateway_mcp_server.h"
#include "gateway_llm_api.h"
#include "gateway_a2a_handler.h"

#ifdef AIRY_HAS_PROTOCOLS
#include "protocol_registry.h"
#endif

#include <signal.h>
#include <stdio.h> /* snprintf：运行期端口事实的文本化 */
#ifndef _WIN32
#include <unistd.h> /* write() in async-signal-safe handler */
#endif

static atomic_int *s_running = NULL;
static daemon_bootstrap_sd_t *s_bsd = NULL;

/**
 * @brief Signal handler (async-signal-safe: only sets an atomic flag; the
 *        actual stop action is done by the main loop)
 *
 * Must not call lock/alloc/log inside a signal handler (airy_mtx_lock,
 * gateway_service_stop etc. are not async-signal-safe); otherwise a signal
 * received while the main loop holds a lock would deadlock.
 */
static void gw_on_signal(int sig)
{
    atomic_store_explicit(s_running, 0, memory_order_seq_cst);
#ifndef _WIN32
    {
        static const char sig_msg[] =
            "[SIG] shutdown signal received, initiating graceful shutdown\n";
        ssize_t written = write(STDERR_FILENO, sig_msg, sizeof(sig_msg) - 1);
        (void)written; /* best-effort diagnostics inside a signal handler */
    }
#endif
}

static void gw_log_toggle(int sig)
{
    (void)sig;
    static int debug_mode = 0;
    debug_mode = !debug_mode;
    log_set_module_level("*", debug_mode ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
}

#ifdef _WIN32
/**
 * @brief Windows console event handler
 */
static BOOL WINAPI gw_on_console(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        gw_on_signal((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

void gw_sig_install(atomic_int *running)
{
    s_running = running; /* bind before registering: no install race */
#ifdef _WIN32
    SetConsoleCtrlHandler(gw_on_console, TRUE);
#else
    signal(SIGINT, gw_on_signal);
    signal(SIGTERM, gw_on_signal);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, gw_log_toggle);
#endif
}

void gw_rpc_stop(void *user_data)
{
    atomic_int *running = user_data;
    if (running)
        atomic_store_explicit(running, 0, memory_order_seq_cst);
}

int gw_plat_boot(void)
{
    /* WS-8 stage 4 (8.4.1): corekern first boot link; idempotent,
     * failure degrades to platform fallbacks (non-fatal, badge=0). */
    int core_ret = airy_init();
    if (core_ret == AIRY_SUCCESS) {
        SVC_LOG_INFO("corekern core initialized (gateway_d runs on corekern)");
    } else {
        SVC_LOG_WARN("corekern init failed (%d) - running degraded (badge=0)", core_ret);
    }

    /* 安全穹顶不可用即拒启：网关是所有 RPC 的入口，绕过策略面运行
     * 等于把整条链路暴露在无鉴权状态。 */
    if (daemon_dome_init_pep("gateway_d") != AIRY_OK) {
        SVC_LOG_ERROR("gateway_d: security dome init failed — refusing to start");
        return -1;
    }
    /* IPC/RPC/SD ops 表进程级幂等发布，init 失败非致命。 */
    daemon_ipc_ops_init("gateway_d");
    daemon_heapstore_init("gateway_d");
    return 0;
}

#ifdef AIRY_HAS_PROTOCOLS
/**
 * @brief Resolve the MCP adapter through the shared protocol registry.
 *
 * The gateway is mechanism: it consumes the MCP adapter via injection
 * (proto_registry) instead of deep-including the vendor adapter header.
 * Registration is idempotent, so the same entry (and context pointer) is
 * returned on every call within the process.
 */
static const protocol_adapter_t *gw_mcp_adapter(void **out_context)
{
    if (proto_interface_register_builtins() != AIRY_SUCCESS)
        return NULL;

    protocol_registry_t *registry = proto_registry_get();
    proto_registry_entry_t *entry = registry ? proto_registry_find(registry, "MCP") : NULL;
    if (!entry || !entry->adapter)
        return NULL;

    if (out_context)
        *out_context = entry->context;
    return entry->adapter;
}
#endif

int gw_proto_wire(gw_proto_router_t *router, gateway_business_ctx_t *biz)
{
    if (gw_proto_router_init(router) != 0) {
        SVC_LOG_ERROR("Failed to init protocol router");
        return -1;
    }

    /* Phase 2: adapter wiring — MCP tools -> tool_d / LLM API -> llm_d /
     * A2A -> sched_d (protocol translation is concentrated in the gateway;
     * daemons have zero protocol knowledge, D2) */
    {
        gw_mcp_server_t *mcp = gw_proto_router_get_mcp(router);
        if (mcp) {
            /* 内置工具目录唯一权威在 tool_d（registry + list_tools 输出
             * input_schema），gateway 启动时拉取注册（M1-1a SSoT 收敛）。 */
            int t_failed = gw_biz_mcp_register_tools(mcp, biz);
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

        /* LLM API: chat/completions → llm_d.complete */
        gw_llm_api_t *llm_api = gw_proto_get_llm(router);
        if (llm_api) {
            gw_llm_set_call(llm_api, gw_biz_llm_complete, biz);
            /* LLM API: embeddings → llm_d.embeddings（RAG/知识库生态接入点） */
            gw_llm_set_embed(llm_api, gw_biz_llm_embeddings, biz);
            /* GET /v1/models 与 chat/embeddings 共用同一默认模型解析结果，
             * 避免模型清单与请求期 fallback 各写一份而漂移。 */
            if (biz && biz->default_model[0])
                gw_llm_set_model(llm_api, biz->default_model);
            SVC_LOG_INFO("Phase 2: LLM API adapter wired — chat/completions + embeddings → llm_d");
        }

        gw_a2a_handler_t *a2a = gw_proto_router_get_a2a(router);
        if (a2a) {
            static const char *a2a_task_types[] = {"coding",  "analysis", "summarize",
                                                   "general", "devops",   NULL};
            for (int i = 0; a2a_task_types[i]; i++) {
                gw_a2a_handler_register_task_type(a2a, a2a_task_types[i],
                                                  gw_biz_sched_schedule, biz);
            }
            SVC_LOG_INFO("Phase 2: A2A adapter wired — task → sched_d");
        }
    }

    /* Initialize UnifiedProtocol stack for multi-protocol support */
#ifdef AIRY_HAS_PROTOCOLS
    {
        void *mcp_context = NULL;
        const protocol_adapter_t *mcp_adapter = gw_mcp_adapter(&mcp_context);
        if (mcp_adapter) {
            if (mcp_adapter->init(mcp_context) == 0) {
                SVC_LOG_INFO("MCP v1.0 adapter initialized (version=%s, caps=0x%x)",
                             mcp_adapter->version ? mcp_adapter->version : "unknown",
                             mcp_adapter->capabilities ? mcp_adapter->capabilities(mcp_context) : 0);
            } else {
                SVC_LOG_WARN("Failed to initialize MCP v1.0 adapter");
            }
        } else {
            SVC_LOG_WARN("MCP v1.0 adapter not available");
        }
    }
#endif

    return 0;
}

static void gw_proto_stop(void)
{
#ifdef AIRY_HAS_PROTOCOLS
    {
        void *mcp_context = NULL;
        const protocol_adapter_t *mcp_adapter = gw_mcp_adapter(&mcp_context);
        if (mcp_adapter && mcp_adapter->destroy) {
            mcp_adapter->destroy(mcp_context);
            SVC_LOG_INFO("MCP adapter destroyed");
        }
    }
#endif
}

/* 运行期端口事实由绑定方发布：run/gateway.port 是输出而非输入。bootstrap /
 * supervisor 直启路径不经过启动器，若不自发布，按该文件解析端点的客户端会
 * 沿用上一次栈的陈旧端口（实测：文件 8080，实监听 2027）。 */
static void gw_publish_port(uint16_t port)
{
    const char *dir = airy_runtime_dir();
    char path[512];
    char buf[16];
    int n;

    if (!dir || !*dir)
        return;
    if (snprintf(path, sizeof(path), "%s/gateway.port", dir) >= (int)sizeof(path)) {
        SVC_LOG_WARN("runtime dir too long, gateway port unpublished");
        return;
    }
    /* 读者（CLI / 启动器 / 洁净房用例）与本进程无同步：走同目录 tmp +
     * rename 落地，任何人读到的要么是旧端口、要么是新端口，不会是半截数字。 */
    n = snprintf(buf, sizeof(buf), "%u\n", (unsigned)port);
    if (n <= 0 || airy_io_write_file(path, buf, (size_t)n) != 0)
        SVC_LOG_WARN("cannot publish gateway port to %s", path);
}

void gw_sd_announce(gateway_service_t service)
{
    /* 端点摘要与 SD/IPC 注册必须读 service 实际生效配置（bind 侧
     * fail-closed 收敛发生在 start 内部，本地 config 副本不反映改写） */
    const gateway_service_config_t *run_cfg = gateway_get_config(service);

    gw_publish_port(run_cfg->http.port);

    SVC_LOG_INFO("AgentRT Gateway Daemon started");
    SVC_LOG_INFO("  HTTP:     %s:%d %s", run_cfg->http.host, run_cfg->http.port,
                 run_cfg->http.enabled ? "[enabled]" : "[disabled]");
    SVC_LOG_INFO("  WebSocket: %s:%d %s", run_cfg->ws.host, run_cfg->ws.port,
                 run_cfg->ws.enabled ? "[enabled]" : "[disabled]");
    SVC_LOG_INFO("  Stdio:    %s", run_cfg->stdio.enabled ? "[enabled]" : "[disabled]");

    s_bsd = daemon_bootstrap_sd_start("gateway_d", "gateway", run_cfg->http.host,
                                      run_cfg->http.port, "gateway,core", 0);
}

void gw_health_tick(gateway_service_t service)
{
    static const int HEALTH_CHECK_INTERVAL = 30;
    static int ticks = 0;
    const gateway_service_config_t *run_cfg = gateway_get_config(service);
    airy_svc_stats_t stats;

    if (!run_cfg->enable_metrics || (++ticks % HEALTH_CHECK_INTERVAL) != 0)
        return;

    if (gateway_service_get_stats(service, &stats) == AIRY_SUCCESS) {
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

void gw_teardown(gateway_service_t service, gw_proto_router_t *router,
                 gateway_business_ctx_t *biz, bool started)
{
    if (started) {
        daemon_bootstrap_sd_stop(s_bsd);
        SVC_LOG_INFO("Gateway shutting down...");
        gateway_service_stop(service, false);
    }

    gw_proto_stop();

#ifndef _WIN32
    gw_mcp_client_cleanup();
#endif
    if (router)
        gw_proto_router_destroy(router);
    if (biz)
        gateway_business_ctx_destroy(biz);
    gateway_service_destroy(service);

    /* 全进程关停级联（main 退出唯一出口）：socket 面 → ops 逆序 → log。 */
    airy_sock_cleanup();
    SVC_LOG_INFO("Gateway daemon stopped");
    daemon_ipc_ops_cleanup();
    daemon_heapstore_cleanup();
    daemon_dome_cleanup();
    log_cleanup();
}
