// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file service.h
 * @brief 工具服务内部结构声明
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef TOOL_SERVICE_INTERNAL_H
#define TOOL_SERVICE_INTERNAL_H

#include "cache.h"
#include "config.h"
#include "executor.h"
#include "daemon_platform_ext.h"
#include "registry.h"
#include "tool_service.h"
#include "validator.h"

struct tool_service {
    tool_registry_t *registry;
    tool_executor_t *executor;
    tool_validator_t *validator;
    tool_cache_t *cache;
    tool_config_t *manager;
    airy_mtx_t lock;
    /* L2 get_stats 真实统计（volatile：宽松一致即可满足监控语义） */
    volatile uint64_t exec_total;    /* 工具执行总次数（含缓存命中） */
    volatile uint64_t exec_fail;     /* 执行失败次数 */
    volatile uint64_t exec_ms_total; /* 执行累计耗时（毫秒） */
};

#endif /* TOOL_SERVICE_INTERNAL_H */