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
#include "daemon_event_driver.h"
#include "rpc/tool_service_internal.h"

tool_service_t *g_service = NULL;

/* D8 工具可用性探测周期（架构改进方案 §3.6.3）：2s。验收要求「kill daemon
 * 后 5s 内其工具从 agent 可见列表消失」，取 2s 留一倍余量；AF_UNIX connect
 * 为本地微秒级调用，轮询成本可忽略。 */
#define TOOL_AVAIL_INTERVAL_MS 2000

/* 可用性视图单例与线程池借用句柄：视图所有权归 tool_service，本件只持
 * 引用做周期探测（服务销毁时随 service 释放，故此处只置 NULL 不 free）。 */
static tool_avail_t *g_avail = NULL;
static thread_pool_t *g_avail_pool = NULL;

static void avail_poll_task(void *arg)
{
    (void)arg;
    tool_avail_poll(g_avail);
}

/* 周期定时器回调（事件循环线程）：只做一次 offload，绝不在事件循环内
 * 做 connect；线程池不可用时降级为就地探测（本地 connect 微秒级）。 */
static void avail_tick(airy_event_loop_t *loop, uint64_t timer_id, void *user_data)
{
    (void)loop;
    (void)timer_id;
    (void)user_data;

    if (!g_avail)
        return;

    if (g_avail_pool) {
        if (thread_pool_submit(g_avail_pool, avail_poll_task, NULL) != 0)
            SVC_LOG_WARN("C-L06: TOOL-AVAIL: probe offload rejected, skipping this round");
        return;
    }
    avail_poll_task(NULL);
}

static void ep_load(const char *config_path)
{
    daemon_ep_load(daemon_ep_slot(), config_path, TOOL_D_SOCKET_UNIX,
                   TOOL_D_SOCKET_WIN, TOOL_D_TCP_PORT, NULL, NULL);
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
    (void)bsd;

    g_avail = tool_service_avail(g_service);
    if (!g_avail) {
        SVC_LOG_ERROR("C-L06: TOOL-AVAIL: view unavailable, exposure filtering disabled");
        return 0;
    }

    g_avail_pool = daemon_event_driver_get_pool(driver);

    /* 首轮同步探测：AF_UNIX connect 为本地微秒级，关闭"先暴露后探测"窗口 */
    tool_avail_poll(g_avail);
    daemon_event_driver_add_timer(driver, TOOL_AVAIL_INTERVAL_MS, avail_tick, NULL);
    SVC_LOG_INFO("C-L06: TOOL-AVAIL: probe timer registered (interval=%dms)",
                 TOOL_AVAIL_INTERVAL_MS);
    return 0;
}

void svc_destroy(void)
{
    /* 可用性视图随 service 释放，本件只解引用（先于 service 置空，避免
     * 定时器在服务销毁后被回调访问悬垂句柄）。 */
    g_avail = NULL;
    g_avail_pool = NULL;

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
