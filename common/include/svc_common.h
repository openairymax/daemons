// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file svc_common.h
 * @brief 服务公共定义（daemons 重导出兼容头）
 *
 * P0.17 阶段 3：实际定义已迁移至 commons/utils/ipc/include/svc_common.h，
 * 消除 atoms→daemons 编译期反向依赖（IRON-6）。本文件保留为重导出
 * 兼容头，使 daemons 内部源文件无需立即修改 #include 路径。
 *
 * 额外包含 daemon_errors.h 提供 daemons 扩展错误码（DAEMON_E* 别名），
 * commons 版 svc_common.h 不包含此依赖。
 *
 * @see commons/utils/ipc/include/svc_common.h (commons 权威版本)
 */

#ifndef AGENTRT_DAEMON_COMMON_SVC_COMMON_H
#define AGENTRT_DAEMON_COMMON_SVC_COMMON_H

/* daemons 扩展错误码（DAEMON_EINIT/ESTATE/EHEALTH 等兼容别名） */
#include "daemon_errors.h"

/* 使用相对路径避免找到 daemons 自身的 svc_common.h（递归包含） */
#include "../../../commons/utils/ipc/include/svc_common.h"

#endif /* AGENTRT_DAEMON_COMMON_SVC_COMMON_H */
