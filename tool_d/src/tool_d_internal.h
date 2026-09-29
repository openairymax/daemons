/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file tool_d_internal.h
 * @brief tool_d 守护进程各编译单元共享的内部声明
 *        （svc.c / tool_rpc.c）。
 * @details RPC 方法入口（m_*）与生命周期钩子声明在生成的
 *          svc_tool_d.h（L3 SSoT）；本头仅承载 daemon 全局 tool 服务
 *          句柄。仅限 tool_d 守护进程编译单元使用。
 */

#ifndef AIRY_RT_DAEMON_TOOL_D_INTERNAL_H
#define AIRY_RT_DAEMON_TOOL_D_INTERNAL_H

#include "tool_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Daemon-wide tool service handle (owned/lifecycled in svc.c,
 * referenced by the JSON-RPC handlers). */
extern tool_service_t *g_service;

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_TOOL_D_INTERNAL_H */
