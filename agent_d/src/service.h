// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
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

/* ---------- invoke 会话（跨进程取消基础，改进1 "取消下探"） ---------- */

/* 活跃 invoke 会话上限（并发取消查找为线性扫描，上限防资源失控）。
 * 每会话持有独立 cancel_token：handle_invoke 注册、agent.cancel 查表取消。 */
#define AGENT_INVOKE_SESSIONS_MAX 1024

typedef struct {
    char request_id[64];   /* 调用方生成的唯一请求 ID */
    airy_cancel_token_t *token; /* invoke 期间活跃的取消令牌（BORROW，调用方生命周期） */
    int active;
} agent_invoke_session_t;

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

/* 槽位状态机：
 *   0 = 空闲（可复用，spawn 失败回滚后）
 *   1 = running（已注册到哈希表，子进程存活）
 *   3 = terminated（terminate 后不回收槽位，保持原语义）
 *   4 = spawning（槽位已预占，子进程启动中，尚未注册哈希表） */
#define AGENT_STATUS_FREE 0
#define AGENT_STATUS_RUNNING 1
#define AGENT_STATUS_TERMINATED 3
#define AGENT_STATUS_SPAWNING 4

typedef struct {
    char *agent_id;       /* Agent 唯一标识（32 字符十六进制） */
    char *spec;            /* Agent 规格（JSON 字符串） */
    int status;            /* 见上方状态机 */
    uint64_t spawned_at;   /* 派生时间戳（秒） */
    /* 细粒度锁：保护本条目的 status/child 句柄，避免全局锁持有期间
     * 做 fork / 子进程 IO 而串行化所有 agent 操作。每个 spawn/invoke/
     * terminate 仅在索引查找时短暂持有全局锁，子进程生命周期操作
     * 在本锁保护下进行，从而支持上千 Agent 真正并行。 */
    airy_mtx_t entry_lock;
#if AIRY_PLATFORM_POSIX
    /* Stage5+ 待办4：真实 spawn — fork Agent runner 子进程后的句柄
     * （Python/Rust 双语言支持）。
     * child_pid>0 表示有活跃子进程；-1 表示无子进程（回退旧逻辑）。
     * stdin_fd 用于向子进程写请求，stdout_fd 用于读响应。
     * last_active：最近一次成功与子进程通信的时间（秒），空闲回收依据。 */
    pid_t child_pid;
    int stdin_fd;
    int stdout_fd;
    uint64_t last_active;
#endif
} agent_entry_internal_t;

struct agent_service {
    agent_entry_internal_t *agents;
    size_t agent_count;
    size_t max_agents;
    agent_hash_table_t agent_index;
    /* 全局锁：仅保护 agent_count、哈希表与槽位分配（快路径）。
     * 禁止在持有本锁期间做 fork / 网络 / 子进程 IO 等阻塞操作。 */
    airy_mtx_t lock;
    int initialized;

    /* ---- 性能监控计数器（10000 并发验证，原子无锁更新） ----
     * 10000 并发下逐请求打日志会刷爆 IO，因此只做原子计数与耗时
     * 聚合（累计/最大），由 agent_d 的 perf 监控线程周期采样输出。
     * - 计数：spawn/invoke/terminate 请求总数与成败
     * - lock_wait_total：全局锁 trylock 探测失败的次数（锁竞争信号）
     * - 耗时：以微秒聚合，64 位防止累计溢出 */
    airy_atomic_int_t m_spawn_total;
    airy_atomic_int_t m_spawn_ok;
    airy_atomic_int_t m_spawn_fail;
    airy_atomic_int_t m_invoke_total;
    airy_atomic_int_t m_invoke_ok;
    airy_atomic_int_t m_invoke_fail;
    airy_atomic_int_t m_terminate_total;
    airy_atomic_int_t m_lock_wait_total;
    airy_atomic_int_t m_peak_running;
    atomic_ullong m_spawn_us_total;
    atomic_ullong m_spawn_us_max;
    atomic_ullong m_invoke_us_total;
    atomic_ullong m_invoke_us_max;

    /* ---- invoke 会话表（改进1 "取消下探"：跨进程取消） ----
     * 保护锁独立于 svc->lock（invoke 路径持 entry_lock 做子进程 IO，
     * 会话注册/注销/取消为短临界区，独立锁避免全局锁长时间占用）。 */
    airy_mtx_t session_lock;
    agent_invoke_session_t sessions[AGENT_INVOKE_SESSIONS_MAX];
};

#endif /* AGENT_SERVICE_INTERNAL_H */
