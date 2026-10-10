// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file skill_rpc.h
 * @brief 技能机制 RPC 服务面声明。
 *
 * atoms/syscall 技能机制（airy_sys_skill_*）的 daemon 服务面：在
 * tool.* 命名空间登记 skill.install/list/execute/uninstall 方法，
 * 供 gateway/CLI 经 JSON-RPC 安装与执行生态技能资产
 * （ecosystem/skills，经 file: URL 安装）。
 */

#ifndef AIRY_RT_TOOL_D_SKILL_RPC_H
#define AIRY_RT_TOOL_D_SKILL_RPC_H

#ifdef __cplusplus
extern "C" {
#endif

/* 在 method dispatcher 上登记 skill.* 方法族。 */
void skill_rpc_register(void *dispatcher);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_D_SKILL_RPC_H */
