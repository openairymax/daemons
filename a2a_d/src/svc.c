/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file svc.c
 * @brief a2a_d 生命周期策略域（端点装载 / 服务装配）。
 *
 * 端点三元组经 daemon_ep_load（daemon_cfg_file 机制件）自 -c JSON 的
 * daemon 段装载并叠加内置缺省；cmdline --tcp 融合（daemon_ep_apply，
 * 只升不降）同样由机制件承担。容量上限不在此域持有：服务核经
 * proto_registry 注入式消费 A2A 适配器，由适配器自持其运行状态。
 */

#include "daemon_cfg_file.h"
#include "svc_a2a_d.h"
#include "a2a_d_internal.h"
#include "platform.h"
#include "svc_logger.h"

a2a_service_t *g_service = NULL;

void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_apply(ep, cmdline_tcp);
}

int svc_prepare(const char *config_path)
{
    daemon_ep_load(daemon_ep_slot(), config_path, A2A_D_SOCKET_UNIX,
                   A2A_D_SOCKET_WIN, A2A_D_TCP_PORT, NULL, NULL);
    SVC_LOG_INFO("A2A service starting, manager=%s", config_path ? config_path : "default");

    g_service = a2a_service_create();
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create a2a service");
        return -1;
    }
    return 0;
}

void svc_destroy(void)
{
    if (g_service) {
        a2a_service_destroy(g_service);
        g_service = NULL;
    }
    daemon_ep_free(daemon_ep_slot());
}
