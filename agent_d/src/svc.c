/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief agent_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * 服务单例 + daemon 配置解析链 + 生命周期五钩子 + 12 个 RPC handler
 * 声明（实现散布 agent_d_rpc.c / agent_run_rpc.c）。配置链：缺省值 →
 * AIRY_MAX_AGENTS env → config 文件 daemon 段覆盖；端点四元组由
 * svc_endpoint 从解析结果回填，cmdline use_tcp 只升不降。业务逻辑在
 * 服务域（service_*.c）与 run 引擎域（agent_run_*.c）。
 */

#include "svc_agent_d.h"

#include "agent_d_internal.h"
#include "airy_memory.h"
#include "daemon_cfg_file.h"
#include "platform.h"

#include <stdlib.h>
#include <time.h>

agent_service_t *g_service = NULL;
uint64_t g_start_time = 0;
agent_daemon_config_t g_config = {0};

#define AGENT_MAX_CLIENTS 2048
#define AGENT_DEFAULT_MAX_AGENTS 10000

static void cfg_on_load(cJSON *root, void *ud)
{
    daemon_ep_cfg_t *ep = (daemon_ep_cfg_t *)ud;
    cJSON *daemon_cfg = cJSON_GetObjectItem(root, "daemon");
    if (!daemon_cfg)
        return;
    daemon_ep_parse(daemon_cfg, ep);
    cJSON *item = cJSON_GetObjectItem(daemon_cfg, "max_clients");
    if (cJSON_IsNumber(item))
        g_config.max_clients = item->valueint;
    item = cJSON_GetObjectItem(daemon_cfg, "max_agents");
    if (cJSON_IsNumber(item))
        g_config.max_agents = (size_t)item->valuedouble;
}

static void config_load(const char *config_path)
{
    daemon_ep_cfg_t ep;
    daemon_ep_def(AGENT_D_SOCKET_UNIX, AGENT_D_SOCKET_WIN, AGENT_D_TCP_PORT, &ep);
    g_config.max_clients = AGENT_MAX_CLIENTS;
    g_config.max_agents = AGENT_DEFAULT_MAX_AGENTS;

    const char *env = getenv("AIRY_MAX_AGENTS");
    if (env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 65536)
            g_config.max_agents = (size_t)v;
    }

    daemon_cfg_read(config_path, cfg_on_load, &ep);
    g_config.socket_path = ep.socket_path;
    g_config.tcp_port = (uint16_t)ep.tcp_port;
    g_config.use_tcp = ep.use_tcp;
}

static void config_free(void)
{
    AIRY_FREE(g_config.socket_path);
    AIRY_MEMSET(&g_config, 0, sizeof(g_config));
}

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    ep->use_tcp = cmdline_tcp || g_config.use_tcp;
    ep->tcp_host = "127.0.0.1";
    ep->tcp_port = (int)g_config.tcp_port;
    ep->sock_unix = g_config.socket_path;
    ep->sock_win = g_config.socket_path;
}

int svc_prepare(const char *config_path)
{
    airy_paths_init();
    g_start_time = (uint64_t)time(NULL);
    config_load(config_path);
    SVC_LOG_INFO("Agent service starting, manager=%s",
                 config_path ? config_path : "default");
    g_service = agent_service_create(g_config.max_agents);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create agent service");
        return -1;
    }
    return 0;
}

int svc_activate(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd)
{
    (void)bsd;
#if AIRY_PLATFORM_POSIX
    idle_reaper_start();
    perf_monitor_start(driver);
#else
    (void)driver;
#endif
    return 0;
}

void svc_teardown(void)
{
    /* 优雅排水：事件循环停止后在途 invoke worker 仍阻塞读子进程（至
     * invoke 超时）；线程池 join 会等满该超时（SIGTERM 挂死），且
     * runner 子进程向已关闭管道写响应。按 agent.cancel 同一请求级
     * token 路径全会话取消：worker 在 select 轮询片内醒来，级联终止
     * 子进程（SIGTERM→2s→SIGKILL）后收尾。 */
    int ncanceled = agent_service_invoke_cancel_all(g_service);
    if (ncanceled > 0) {
        SVC_LOG_WARN("Shutdown: canceled %d in-flight invoke session(s), draining",
                     ncanceled);
        agent_service_invoke_wait_idle(g_service, 10000);
    }
#if AIRY_PLATFORM_POSIX
    perf_monitor_stop();
    idle_reaper_stop();
#endif
}

void svc_destroy(void)
{
    if (g_service) {
        agent_service_destroy(g_service);
        g_service = NULL;
    }
    config_free();
}

/* 无静态表外动态注册（manifest methods 全量覆盖），空实现 */
void svc_attach(void *dispatcher)
{
    (void)dispatcher;
}
