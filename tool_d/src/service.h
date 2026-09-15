/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file service.h
 * @brief Tool-service internal structure declarations.
 */

#ifndef TOOL_SERVICE_INTERNAL_H
#define TOOL_SERVICE_INTERNAL_H

#include "cache.h"
#include "config.h"
#include "executor.h"
#include "executor_pool.h"
#include "daemon_platform_ext.h"
#include "registry.h"
#include "tool_service.h"
#include "validator.h"

#include <stdatomic.h>

struct tool_service {
    tool_registry_t *registry;
    tool_executor_t *executor;
    executor_pool_t *exec_pool; /* R1-a: 执行面隔离池 */
    tool_validator_t *validator;
    tool_cache_t *cache;
    tool_config_t *manager;
    airy_mtx_t lock;

    /* 会话线程并发自增（跨会话并发执行），必须原子 */
    _Atomic uint64_t exec_total;
    _Atomic uint64_t exec_fail;
    _Atomic uint64_t exec_ms_total;
};

#endif /* TOOL_SERVICE_INTERNAL_H */