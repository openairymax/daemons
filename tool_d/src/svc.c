/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief tool_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * tool 服务单例 + 生命周期五钩子。可配置户：端点基线取自生成头常量
 * （TOOL_D_SOCKET_UNIX/WIN、TOOL_D_TCP_PORT），端点族委托 daemon_cfg_file
 * 机制件（daemon_ep_load/free/apply，纯端点户无策略键）。插件执行域
 * （dlopen）随迁 tool_d，权限/发现/扫描加载在本进程内初始化，生命周期
 * 由本文件承载；业务逻辑在 tool_service_*.c / builtin*.c 与 tool_rpc.c。
 */

#include "platform.h"
#include "plugin/plugin_rpc.h"
#include "rpc/skill_rpc.h"
#include "svc_logger.h"
#include "svc_tool_d.h"
#include "tool_d_internal.h"

#include "daemon_cfg_file.h"

tool_service_t *g_service = NULL;

static void ep_load(const char *config_path)
{
    daemon_ep_load(daemon_ep_slot(), config_path, TOOL_D_SOCKET_UNIX,
                   TOOL_D_SOCKET_WIN, TOOL_D_TCP_PORT, NULL, NULL);
}

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_apply(ep, cmdline_tcp);
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

void svc_destroy(void)
{
    /* 插件执行域随 tool_d 回收（幂等，可重入 fail_svc 路径） */
    plugin_rpc_cleanup();
    if (g_service) {
        tool_service_destroy(g_service);
        g_service = NULL;
    }
    daemon_ep_free(daemon_ep_slot());
}

/* 策略层附加装配挂点：plugin_* 方法族（dlopen 执行域）与 skill_* 方法族
 * （syscall 技能机制服务面）登记到 tool 命名空间（gateway plugin.* cap
 * 改路由到此）。 */
void svc_attach(void *dispatcher)
{
    plugin_rpc_register(dispatcher);
    skill_rpc_register(dispatcher);
}
