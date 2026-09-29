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
#include "svc_a2a_d.h"
#include "a2a_d_internal.h"
#include "platform.h"
#include "svc_logger.h"
#include "daemon_main.h"

#include <stdio.h>
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

static void cfg_load(const char *config_path)
{
    g_cfg.use_tcp = 0;
    g_cfg.tcp_port = A2A_D_TCP_PORT;
#if defined(AIRY_PLATFORM_WINDOWS)
    g_cfg.socket_path = AIRY_STRDUP(A2A_D_SOCKET_WIN);
#else
    g_cfg.socket_path = AIRY_STRDUP(A2A_D_SOCKET_UNIX);
#endif

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

    if (config_path) {
        FILE *f = fopen(config_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len > 0 && len < 1024 * 1024) {
                char *content = (char *)AIRY_MALLOC((size_t)len + 1);
                if (content) {
                    size_t read_len = fread(content, 1, (size_t)len, f);
                    if (read_len == (size_t)len) {
                        content[read_len] = '\0';
                        do {
                            CJSON_PARSE_GUARD(root, content, { break; });
                            cJSON *daemon_cfg = cJSON_GetObjectItem(root, "daemon");
                            if (daemon_cfg) {
                                cJSON *socket_path =
                                    cJSON_GetObjectItem(daemon_cfg, "socket_path");
                                if (cJSON_IsString(socket_path)) {
                                    AIRY_FREE(g_cfg.socket_path);
                                    g_cfg.socket_path = AIRY_STRDUP(socket_path->valuestring);
                                }
                                cJSON *tcp_port = cJSON_GetObjectItem(daemon_cfg, "tcp_port");
                                if (cJSON_IsNumber(tcp_port)) {
                                    g_cfg.tcp_port = (uint16_t)tcp_port->valueint;
                                    g_cfg.use_tcp = 1;
                                }
                                cJSON *max_agents = cJSON_GetObjectItem(daemon_cfg, "max_agents");
                                if (cJSON_IsNumber(max_agents))
                                    g_cfg.max_agents = (size_t)max_agents->valuedouble;
                                cJSON *max_tasks = cJSON_GetObjectItem(daemon_cfg, "max_tasks");
                                if (cJSON_IsNumber(max_tasks))
                                    g_cfg.max_tasks = (size_t)max_tasks->valuedouble;
                            }
                        } while (0);
                    }
                    AIRY_FREE(content);
                }
            }
            fclose(f);
        }
    }
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
