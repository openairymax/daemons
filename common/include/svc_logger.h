// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file svc_logger.h
 * @brief 日志服务兼容层（重导出）
 *
 * P0.17 阶段 2：实际定义已迁移至 commons/utils/logging/include/svc_logger.h，
 * 消除 atoms→daemons 编译期反向依赖（IRON-6）。本文件保留为重导出
 * 兼容头，使 daemons 内部源文件无需立即修改 #include 路径。
 *
 * @see commons/utils/logging/include/svc_logger.h
 */

#ifndef AIRY_RT_DAEMON_COMMON_SVC_LOGGER_H
#define AIRY_RT_DAEMON_COMMON_SVC_LOGGER_H

/* P0.17 阶段 2: 包含 daemon 扩展错误码（DAEMON_EINIT/ESTATE/EHEALTH 等）。
 * 迁移前 daemons 版 svc_logger.h 间接包含 daemons 版 error.h 提供这些错误码，
 * 迁移后 commons 版 svc_logger.h 不再包含 daemons 版 error.h，需在此显式包含。 */
#include "daemon_errors.h"

/* P0.17 阶段 2: 需要 daemons 特有函数声明（airy_dl_*、airy_sock_* 等）
 * 的源文件应直接 #include "daemon_platform_ext.h"，不通过 svc_logger.h 间接获取。
 * 参见 daemon_platform_ext.h 了解 daemons 特有平台扩展声明。 */

/* 使用相对路径避免找到 daemons 自身的 svc_logger.h（递归包含） */
#include "../../../commons/utils/logging/include/svc_logger.h"

#endif /* AIRY_RT_DAEMON_COMMON_SVC_LOGGER_H */
