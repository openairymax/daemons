// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file plugin_rpc.h
 * @brief 插件执行域 RPC 服务（0.1.9 M4：plugin_d → tool_d 整编）。
 *
 * tool_d 内承载插件 dlopen 执行域：plugin_rpc_init 初始化权限/发现/
 * 扫描，plugin_rpc_register 在 tool.* 命名空间登记 plugin_* 方法，
 * plugin_rpc_cleanup 对称回收。gateway plugin.* cap 改路由到 tool
 * 命名空间（旧 namespace 保留转发）。
 */

#ifndef AIRY_RT_TOOL_D_PLUGIN_RPC_H
#define AIRY_RT_TOOL_D_PLUGIN_RPC_H

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化插件执行域（权限 + 发现 + 扫描加载）。返回 0 成功。 */
int plugin_rpc_init(void);

/* 对称清理（幂等）。 */
void plugin_rpc_cleanup(void);

/* 在 tool_d 方法调度器上登记 plugin_* 方法（12 个）。 */
void plugin_rpc_register(void *disp);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_D_PLUGIN_RPC_H */
