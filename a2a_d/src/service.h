// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file service.h
 * @brief A2A 服务内部结构声明
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef A2A_SERVICE_INTERNAL_H
#define A2A_SERVICE_INTERNAL_H

#include "a2a_service.h"

#include "platform.h"

#include <a2a_v03_adapter.h>

#include <stddef.h>
#include <stdint.h>

struct a2a_service {
    a2a_v03_context_t *ctx;   /* A2A v0.3 协议上下文（由 adapter 库持有） */
    airy_mtx_t lock;          /* 线程安全锁 */
    int initialized;          /* 初始化标志 */
    size_t max_agents;        /* 最大智能体数 */
    size_t max_tasks;         /* 最大任务数 */
};

#endif /* A2A_SERVICE_INTERNAL_H */
