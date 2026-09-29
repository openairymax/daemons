/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file monit_d_internal.h
 * @brief monit_d 守护进程各编译单元共享的内部声明
 *        （svc.c / monit_rpc.c）。
 * @details RPC 方法入口（m_*）与生命周期钩子声明在
 *          生成的 svc_monit_d.h（L3 SSoT）；本头仅承载 daemon 全局
 *          monitor 服务句柄与服务起点时间戳。仅限 monit_d 守护进程
 *          编译单元使用。
 */

#ifndef AIRY_RT_DAEMON_MONIT_D_INTERNAL_H
#define AIRY_RT_DAEMON_MONIT_D_INTERNAL_H

#include "monitor_service.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Daemon-wide monitor service handle (owned/lifecycled in svc.c,
 * referenced by the JSON-RPC handlers). */
extern monitor_service_t *g_service;

/* Service start timestamp in seconds (get_stats uptime baseline). */
extern uint64_t g_service_start_time;

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_MONIT_D_INTERNAL_H */
