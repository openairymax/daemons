/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file maths_d_internal.h
 * @brief maths_d 装配域唯一私有头（gen5 五件套⑤）。
 *
 * 域切分：core（服务核心与 RPC 分发）/ expr（表达式求值引擎，自包含）/
 * backend（Python 符号后端，仅 POSIX）。装配域（svc.c）经本头单点消费
 * core 契约；域间依赖单向 core→expr（引擎调用与容量契约）与
 * core→backend（后端句柄），expr 与 backend 零域外依赖。端点常量由生成头
 * svc_maths_d.h 提供（wire 契约 SSoT），本头不重复声明。各域私有头不经
 * 本头外泄，域内实现不外泄。
 */

#ifndef AIRY_RT_DAEMON_MATHS_D_INTERNAL_H
#define AIRY_RT_DAEMON_MATHS_D_INTERNAL_H

#include "core/maths_service.h"

#endif /* AIRY_RT_DAEMON_MATHS_D_INTERNAL_H */
