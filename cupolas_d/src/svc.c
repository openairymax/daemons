/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief cupolas_d 机制层-策略层适配（gen5 装配的策略挂点）。
 *
 * 安全穹顶 PDP 本体 + 生命周期五钩子。端点配置族（daemon 段解析、
 * 基线回填、cmdline use_tcp 融合）委托 daemon_cfg_file 机制件
 * （daemon_ep_load/free/fill）；本文件无策略键，纯端点户。穹顶引导
 * （daemon_cupolas_init，manifest cupolas:"full"）由生成 main.c 承担；
 * 本文件持有动态策略引擎（PDP，M2-S3 唯一策略持有者）与 cupolas
 * 服务单例的创建、注入与销毁。业务逻辑在 service.c 与 cupolas_rpc_*.c。
 */

#include "svc_cupolas_d.h"

#include "cupolas_d_internal.h"
#include "daemon_cfg_file.h"
#include "dynamic_policy_engine.h"
#include "platform.h"

cupolas_service_t *g_service = NULL;
dpolicy_engine_t *g_dpolicy = NULL;

static daemon_ep_cfg_t g_ep;

static void config_load(const char *config_path)
{
    daemon_ep_load(&g_ep, config_path, CUPOLAS_D_SOCKET_UNIX,
                   CUPOLAS_D_SOCKET_WIN, CUPOLAS_D_TCP_PORT, NULL, NULL);
}

static void config_free(void)
{
    daemon_ep_free(&g_ep);
}

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_fill(ep, &g_ep, cmdline_tcp);
}

int svc_prepare(const char *config_path)
{
    airy_paths_init();
    config_load(config_path);
    SVC_LOG_INFO("Cupolas service starting, manager=%s",
                 config_path ? config_path : "default");

    /* PDP：动态策略引擎（M2-S3）——cupolas_d 作为唯一策略持有者 */
    g_dpolicy = dpolicy_engine_create(DPOLICY_CONFLICT_DENY_WINS);
    if (!g_dpolicy) {
        SVC_LOG_ERROR("Failed to create dynamic policy engine");
        return -1;
    }

    g_service = cupolas_service_create(config_path);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create cupolas service");
        dpolicy_engine_destroy(g_dpolicy);
        g_dpolicy = NULL;
        return -1;
    }

    /* M2-S2（0.1.9 §3.2 PDP）：PDP 引擎注入服务层——check_permission
     * 裁决先经动态策略运行集（命中即权威），未命中回退基础 ACL。 */
    cupolas_set_dpolicy(g_service, g_dpolicy);
    return 0;
}

void svc_teardown(void)
{
}

void svc_destroy(void)
{
    if (g_service) {
        cupolas_service_destroy(g_service);
        g_service = NULL;
    }
    if (g_dpolicy) {
        dpolicy_engine_destroy(g_dpolicy);
        g_dpolicy = NULL;
    }
    config_free();
}

/* 无静态表外动态注册（manifest methods 全量覆盖），空实现 */
void svc_attach(void *dispatcher)
{
    (void)dispatcher;
}
