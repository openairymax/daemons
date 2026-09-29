/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief tool_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * tool 服务单例 + 生命周期五钩子。可配置户：端点基线取自生成头常量
 * （TOOL_D_SOCKET_UNIX/WIN、TOOL_D_TCP_PORT），-c JSON 的 daemon 段可
 * 覆盖 socket_path / tcp_port；cmdline use_tcp 只升不降。插件执行域
 * （dlopen）随迁 tool_d，权限/发现/扫描加载在本进程内初始化，生命周期
 * 由本文件承载；业务逻辑在 tool_service_*.c / builtin*.c 与 tool_rpc.c。
 */

#include "daemon_main.h"
#include "platform.h"
#include "plugin_rpc.h"
#include "svc_logger.h"
#include "svc_tool_d.h"
#include "tool_d_internal.h"
#include "tool_service.h"

#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>

tool_service_t *g_service = NULL;

typedef struct {
    char *socket_path;
    uint16_t tcp_port;
    int use_tcp;
} tool_ep_cfg_t;

static tool_ep_cfg_t g_ep = {0};

static void ep_load(const char *config_path)
{
    g_ep.use_tcp = 0;
    g_ep.tcp_port = TOOL_D_TCP_PORT;
#if defined(AIRY_PLATFORM_WINDOWS)
    g_ep.socket_path = AIRY_STRDUP(TOOL_D_SOCKET_WIN);
#else
    g_ep.socket_path = AIRY_STRDUP(TOOL_D_SOCKET_UNIX);
#endif

    if (!config_path)
        return;

    FILE *f = fopen(config_path, "rb");
    if (!f)
        return;

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
                        cJSON *socket_path = cJSON_GetObjectItem(daemon_cfg, "socket_path");
                        if (cJSON_IsString(socket_path)) {
                            AIRY_FREE(g_ep.socket_path);
                            g_ep.socket_path = AIRY_STRDUP(socket_path->valuestring);
                        }
                        cJSON *tcp_port = cJSON_GetObjectItem(daemon_cfg, "tcp_port");
                        if (cJSON_IsNumber(tcp_port)) {
                            g_ep.tcp_port = (uint16_t)tcp_port->valueint;
                            g_ep.use_tcp = 1;
                        }
                    }
                } while (0);
            }
            AIRY_FREE(content);
        }
    }
    fclose(f);
}

static void ep_free(void)
{
    AIRY_FREE(g_ep.socket_path);
    AIRY_MEMSET(&g_ep, 0, sizeof(g_ep));
}

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    ep->use_tcp = cmdline_tcp ? 1 : (g_ep.use_tcp ? 1 : 0);
    ep->tcp_host = "127.0.0.1";
    ep->tcp_port = g_ep.tcp_port;
    ep->sock_unix = g_ep.socket_path;
    ep->sock_win = g_ep.socket_path;
}

int svc_prepare(const char *config_path)
{
    ep_load(config_path);

    /* 插件执行域（dlopen）随迁 tool_d：初始化失败降级为 WARN，
     * 不阻断工具注册表主面。 */
    if (plugin_rpc_init() != 0)
        SVC_LOG_WARN("plugin_rpc_init failed, plugin.* unavailable");

    g_service = tool_service_create(config_path ? config_path
                                                : "agentrt/manager/service/tool_d/tool.yaml");
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create tool service");
        return -1;
    }
    SVC_LOG_INFO("tool service started");
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
    /* 插件执行域随 tool_d 回收（幂等，可重入 fail_svc 路径） */
    plugin_rpc_cleanup();
    if (g_service) {
        tool_service_destroy(g_service);
        g_service = NULL;
    }
    ep_free();
}

/* 策略层附加装配挂点：plugin_* 方法族（dlopen 执行域）登记到
 * tool 命名空间（gateway plugin.* cap 改路由到此）。 */
void svc_attach(void *dispatcher)
{
    plugin_rpc_register(dispatcher);
}
