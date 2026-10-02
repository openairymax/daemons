/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief a2a_d 生命周期策略域（gen5 手写层：config 加载链 / 服务装配）。
 *
 * config 优先级：内置缺省 < env（AIRY_A2A_MAX_AGENTS / AIRY_A2A_MAX_TASKS）
 * < -c JSON daemon 段（socket_path / tcp_port / max_agents / max_tasks，
 * tcp_port 置入即升 use_tcp）。容量缺省（0 回落 256/4096）由服务层单一
 * 持有，此处不重复定义；旧 max_clients 死配置随迁移根除。端点与
 * cmdline --tcp 的融合在 svc_endpoint 完成（只升不降）。
 */

#include "airy_memory.h"
#include "daemon_cfg_file.h"
#include "svc_a2a_d.h"
#include "a2a_d_internal.h"
#include "platform.h"
#include "svc_logger.h"
#include "daemon_main.h"

#include <stdlib.h>

a2a_service_t *g_service = NULL;

typedef struct {
    char *socket_path;
    uint16_t tcp_port;
    int use_tcp;
    size_t max_agents;
    size_t max_tasks;
} a2a_daemon_config_t;

static a2a_daemon_config_t g_cfg = {0};

static void cfg_on_load(cJSON *root, void *ud)
{
    daemon_ep_cfg_t *ep = (daemon_ep_cfg_t *)ud;
    cJSON *daemon_cfg = cJSON_GetObjectItem(root, "daemon");
    daemon_ep_parse(daemon_cfg, ep);
    cJSON *max_agents = cJSON_GetObjectItem(daemon_cfg, "max_agents");
    if (cJSON_IsNumber(max_agents))
        g_cfg.max_agents = (size_t)max_agents->valuedouble;
    cJSON *max_tasks = cJSON_GetObjectItem(daemon_cfg, "max_tasks");
    if (cJSON_IsNumber(max_tasks))
        g_cfg.max_tasks = (size_t)max_tasks->valuedouble;
}

static void cfg_load(const char *config_path)
{
    daemon_ep_cfg_t ep;
    daemon_ep_def(A2A_D_SOCKET_UNIX, A2A_D_SOCKET_WIN, A2A_D_TCP_PORT, &ep);

    const char *env_agents = getenv("AIRY_A2A_MAX_AGENTS");
    if (env_agents) {
        unsigned long v = strtoul(env_agents, NULL, 10);
        if (v > 0 && v < 65536)
            g_cfg.max_agents = (size_t)v;
    }
    const char *env_tasks = getenv("AIRY_A2A_MAX_TASKS");
    if (env_tasks) {
        unsigned long v = strtoul(env_tasks, NULL, 10);
        if (v > 0 && v < 1048576)
            g_cfg.max_tasks = (size_t)v;
    }

    daemon_cfg_read(config_path, cfg_on_load, &ep);

    g_cfg.socket_path = ep.socket_path;
    g_cfg.tcp_port = (uint16_t)ep.tcp_port;
    g_cfg.use_tcp = ep.use_tcp;
}

static void cfg_free(void)
{
    AIRY_FREE(g_cfg.socket_path);
    AIRY_MEMSET(&g_cfg, 0, sizeof(g_cfg));
}

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    ep->use_tcp = cmdline_tcp ? 1 : g_cfg.use_tcp;
    ep->tcp_host = "127.0.0.1";
    ep->tcp_port = g_cfg.tcp_port;
    ep->sock_unix = g_cfg.socket_path;
    ep->sock_win = g_cfg.socket_path;
}

int svc_prepare(const char *config_path)
{
    cfg_load(config_path);
    SVC_LOG_INFO("A2A service starting, manager=%s", config_path ? config_path : "default");

    g_service = a2a_service_create(g_cfg.max_agents, g_cfg.max_tasks);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create a2a service");
        return -1;
    }
    return 0;
}

int svc_activate(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd)
{
    (void)driver;
    (void)bsd;
    return 0;
}

void svc_teardown(void)
{
}

void svc_destroy(void)
{
    if (g_service) {
        a2a_service_destroy(g_service);
        g_service = NULL;
    }
    cfg_free();
}

/* 无静态表外动态注册（manifest methods 全量覆盖），空实现 */
void svc_attach(void *dispatcher)
{
    (void)dispatcher;
}
