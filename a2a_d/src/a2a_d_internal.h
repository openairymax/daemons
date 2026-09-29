/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file a2a_d_internal.h
 * @brief a2a_d 守护进程各编译单元共享的内部声明
 *        （svc.c / a2a_rpc.c）。
 * @details RPC 方法入口（m_*）与生命周期钩子声明在
 *          生成的 svc_a2a_d.h（L3 SSoT）；本头仅承载 daemon 全局
 *          A2A 服务句柄。仅限 a2a_d 守护进程编译单元使用。
 */

#ifndef AIRY_RT_DAEMON_A2A_D_INTERNAL_H
#define AIRY_RT_DAEMON_A2A_D_INTERNAL_H

#include "a2a_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Daemon-wide A2A service handle (owned/lifecycled in svc.c,
 * referenced by the JSON-RPC handlers). */
extern a2a_service_t *g_service;

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_A2A_D_INTERNAL_H */
