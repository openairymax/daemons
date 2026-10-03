/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief a2a_d 生命周期策略域（config 加载链 / 服务装配）。
 *
 * config 优先级：内置缺省 < env（AIRY_A2A_MAX_AGENTS / AIRY_A2A_MAX_TASKS）
 * < -c JSON daemon 段（max_agents / max_tasks，端点三元组随 daemon 段
 * 覆盖）。容量缺省（0 回落 256/4096）由服务层单一持有，此处不重复
 * 定义。端点装载（daemon_ep_load）与 cmdline --tcp 融合
 * （daemon_ep_apply，只升不降）为 daemon_cfg_file 机制件，本文件仅保留
 * env 覆盖与容量键提取策略件。
 */

#include "daemon_cfg_file.h"
#include "svc_a2a_d.h"
#include "a2a_d_internal.h"
#include "platform.h"
#include "svc_logger.h"

#include <stdlib.h>

a2a_service_t *g_service = NULL;

typedef struct {
    size_t max_agents;
    size_t max_tasks;
} a2a_daemon_config_t;

static a2a_daemon_config_t g_cfg = {0};

static void cfg_keys(const cJSON *root, void *user)
{
    (void)user;
    cJSON *daemon_cfg = cJSON_GetObjectItem(root, "daemon");
    cJSON *max_agents = cJSON_GetObjectItem(daemon_cfg, "max_agents");
    if (cJSON_IsNumber(max_agents))
        g_cfg.max_agents = (size_t)max_agents->valuedouble;
    cJSON *max_tasks = cJSON_GetObjectItem(daemon_cfg, "max_tasks");
    if (cJSON_IsNumber(max_tasks))
        g_cfg.max_tasks = (size_t)max_tasks->valuedouble;
}

static void cfg_load(const char *config_path)
{
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

    daemon_ep_load(daemon_ep_slot(), config_path, A2A_D_SOCKET_UNIX,
                   A2A_D_SOCKET_WIN, A2A_D_TCP_PORT, cfg_keys, NULL);
}

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_apply(ep, cmdline_tcp);
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

void svc_teardown(void)
{
}

void svc_destroy(void)
{
    if (g_service) {
        a2a_service_destroy(g_service);
        g_service = NULL;
    }
    daemon_ep_free(daemon_ep_slot());
}

/* 无静态表外动态注册（manifest methods 全量覆盖），空实现 */
void svc_attach(void *dispatcher)
{
    (void)dispatcher;
}
