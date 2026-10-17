/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file a2a_d_internal.h
 * @brief a2a_d 装配域唯一私有头（gen5 五件套⑤）。
 *
 * 域切分：core（服务核，经 proto_registry 注入式消费适配器，编入
 * airy_a2a_service 静态库）/ rpc（a2a.* 方法域，可执行侧装配原子件）。
 * 装配域（svc.c）经本头单点消费 core 契约并持有全局句柄；rpc 域经
 * 本头消费同一句柄，域依赖单向 rpc→core。RPC 方法入口（m_*）与生命
 * 周期钩子声明在生成的 svc_a2a_d.h（L3 SSoT）。仅限 a2a_d 编译单元。
 */

#ifndef AIRY_RT_DAEMON_A2A_D_INTERNAL_H
#define AIRY_RT_DAEMON_A2A_D_INTERNAL_H

#include "core/a2a_service.h"

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
