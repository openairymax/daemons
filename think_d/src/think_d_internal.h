// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file think_d_internal.h
 * @brief think_d 跨文件内部符号声明（RPC 入口唯一归属生成头 svc_think_d.h）。
 */

#ifndef AIRY_RT_DAEMON_THINK_D_INTERNAL_H
#define AIRY_RT_DAEMON_THINK_D_INTERNAL_H

#include "daemon_dep.h"
#include "think_service.h"

#ifdef __cplusplus
extern "C" {
#endif

extern think_service_t *g_svc;

/* SD 探测句柄（svc 域 activate 期缓存，RPC 域健康面共用）；
 * 由生成钩子 svc_activate 的 bsd 参数解析，进程内只读。 */
extern sd_helper_t *g_sdh;

/* 0.1.19 §7.2 不变式 3：硬依赖以数据声明，required 缺失必须降级态显式上报。
 * think_d 的推理执行面依赖 llm_d（LLM 服务）；声明数据由 svc.c 定义，
 * health_check 面与启动期依赖报告共用。 */
extern const daemon_dep_spec_t g_think_deps[1];

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_THINK_D_INTERNAL_H */
