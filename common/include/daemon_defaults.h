/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file daemon_defaults.h
 * @brief 守护进程共享默认值（重导出）
 *
 * P0.17 阶段 1：实际定义已迁移至 commons/include/agentrt_defaults.h，
 * 消除 atoms→daemons 编译期反向依赖（IRON-6）。本文件保留为重导出
 * 兼容头，使 daemons 内部源文件（circuit_breaker.c、api_recovery.c、
 * svc_auth.c、llm_d/service.c、tool_d/service.c 等）无需立即修改
 * #include 路径。后续 SP 任务可逐步将 daemons 内部引用改为直接
 * 引用 agentrt_defaults.h 并删除本兼容头。
 */

#ifndef AGENTRT_DAEMON_DEFAULTS_H
#define AGENTRT_DAEMON_DEFAULTS_H

#include "agentrt_defaults.h"

#endif /* AGENTRT_DAEMON_DEFAULTS_H */
