// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_d_internal.h
 * @brief channel_d 守护进程各编译单元共享的内部声明
 *        （svc.c / channel_rpc.c）。
 * @details RPC 方法入口（svc_on_*_channel_d）与生命周期钩子声明在
 *          生成的 svc_channel_d.h（L3 SSoT）；本头仅承载 daemon 全局
 *          channel 服务句柄。仅限 channel_d 守护进程编译单元使用。
 */

#ifndef AIRY_RT_DAEMON_CHANNEL_D_INTERNAL_H
#define AIRY_RT_DAEMON_CHANNEL_D_INTERNAL_H

#include "channel_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Daemon-wide channel service handle (owned/lifecycled in svc.c,
 * referenced by the JSON-RPC handlers). */
extern channel_service_t *g_svc;

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_CHANNEL_D_INTERNAL_H */
