// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file platform.h
 * @brief 平台抽象兼容层（daemon 专用）— 重导出
 *
 * P0.17 阶段 2：daemons 特有声明已迁移至 daemon_platform_ext.h。
 * 本文件仅作为向后兼容的重导出层。
 *
 * 注意：由于 -I 路径顺序中 commons/platform/include 排在
 * daemons/common/include 之前，daemons 子模块源文件的
 * #include "platform.h" 通常找到 commons 版 platform.h。
 * 需要 daemons 特有函数声明的源文件应直接
 * #include "daemon_platform_ext.h"。
 *
 * @see agentrt/commons/platform/include/platform.h  (commons 权威平台抽象)
 * @see daemon_platform_ext.h                         (daemons 特有扩展)
 */

#ifndef AIRY_RT_DAEMON_COMMON_PLATFORM_H
#define AIRY_RT_DAEMON_COMMON_PLATFORM_H

#include "daemon_platform_ext.h"

#endif /* AIRY_RT_DAEMON_COMMON_PLATFORM_H */
