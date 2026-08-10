// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file ipc_service_bus.h
 * @brief IPC服务总线（daemons 重导出兼容头）
 *
 * P0.17 阶段 3：实际定义已迁移至 commons/utils/ipc/include/ipc_service_bus.h，
 * 消除 atoms→daemons 编译期反向依赖（IRON-6）。本文件保留为重导出
 * 兼容头，使 daemons 内部源文件无需立即修改 #include 路径。
 *
 * @see commons/utils/ipc/include/ipc_service_bus.h (commons 权威版本)
 */

#ifndef AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_H
#define AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_H

/* 使用相对路径避免找到 daemons 自身的 ipc_service_bus.h（递归包含） */
#include "../../../commons/utils/ipc/include/ipc_service_bus.h"

#endif /* AIRY_RT_DAEMON_COMMON_IPC_SERVICE_BUS_H */
