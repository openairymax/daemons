// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file channel_d_internal.h
 * @brief channel_d 装配域唯一私有头（gen5 五件套⑤）。
 *
 * 域切分：rpc（channel.* 方法域）/ service（POSIX 策略与服务契约，
 * Win32 平台钩子 channel_win32.c 同域对偶）/ book（簿记机制域）/
 * io（收发与探测域）。装配域（svc.c）与 rpc 域经本头单点消费
 * service 契约并持有全局句柄；book/io 域直接消费 service 域内部头
 * service/channel_service_internal.h。RPC 方法入口（m_*）与生命周期
 * 钩子声明在生成的 svc_channel_d.h（L3 SSoT）。仅限 channel_d
 * 编译单元。
 * @details Daemon 全局 channel 服务句柄。
 */

#ifndef AIRY_RT_DAEMON_CHANNEL_D_INTERNAL_H
#define AIRY_RT_DAEMON_CHANNEL_D_INTERNAL_H

#include "service/channel_service.h"

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
