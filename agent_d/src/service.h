// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file service.h
 * @brief Agent 服务内部结构声明
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef AGENT_SERVICE_INTERNAL_H
#define AGENT_SERVICE_INTERNAL_H

#include "agent_service.h"

#include "platform.h"

#include <stddef.h>
#include <stdint.h>

/* ---------- 内部哈希表（与 syscall_router.c 解耦的独立实现） ---------- */

typedef struct {
    char *key;
    size_t index;
    int occupied;
} agent_hash_entry_t;

typedef struct {
    agent_hash_entry_t *entries;
    size_t capacity;
    size_t count;
} agent_hash_table_t;

/* ---------- Agent 条目 ---------- */

typedef struct {
    char *agent_id;       /* Agent 唯一标识（32 字符十六进制） */
    char *spec;            /* Agent 规格（JSON 字符串） */
    int status;            /* 1=running, 3=terminated */
    uint64_t spawned_at;   /* 派生时间戳（秒） */
#if AIRY_PLATFORM_POSIX
    /* Stage5+ 待办4：真实 spawn — fork Python runner 子进程后的句柄。
     * child_pid>0 表示有活跃子进程；-1 表示无子进程（回退旧逻辑）。
     * stdin_fd 用于向子进程写请求，stdout_fd 用于读响应。 */
    pid_t child_pid;
    int stdin_fd;
    int stdout_fd;
#endif
} agent_entry_internal_t;

struct agent_service {
    agent_entry_internal_t *agents;
    size_t agent_count;
    size_t max_agents;
    agent_hash_table_t agent_index;
    airy_mtx_t lock;
    int initialized;
};

#endif /* AGENT_SERVICE_INTERNAL_H */
