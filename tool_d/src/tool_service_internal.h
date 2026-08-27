// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file tool_service_internal.h
 * @brief Tool service 拆分文件间的共享声明（2026-08-27）。
 *
 * service.c 按单一职责拆分为三个文件：
 *   - service.c           生命周期/注册表/统计/资源释放/交互审批域
 *   - service_builtin.c   builtin 工具注册与 AIRY_AGENT_ACL 预授权域
 *   - service_execute.c   工具执行域（同步/流式 + 缓存）
 * 被 service_builtin.c 与 service.c 共享的注册函数经此头声明；
 * struct tool_service 定义见 service.h。
 */

#ifndef AIRY_RT_TOOL_SERVICE_INTERNAL_H
#define AIRY_RT_TOOL_SERVICE_INTERNAL_H

#include "service.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Builtin 工具注册域（service_builtin.c） ---- */
void tool_service_register_acl_from_env(void);
void register_builtin_tools(tool_service_t *svc);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_SERVICE_INTERNAL_H */
