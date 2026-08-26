/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file ledger.h
 * @brief Context ledger service（上下文台账，mem.ledger_* 命名空间）。
 *
 * 上下文台账：为每个会话维护「上下文构成的不可变账本」——窗口内每个条目
 * （系统提示/工具定义/用户消息/工具结果/助手消息/压缩块/缓存命中）的 Token
 * 成本与状态流转全程可审计，并提供预算校验，作为提示词压缩的决策数据源。
 *
 * 实现 AirymaxRT 13-semantic-cache-context-ledger.md 第 4 章：
 *   - append-only：状态变更不修改原条目，追加 status 记录保留历史
 *   - 预算：每会话 ctx_budget（默认 32K）与 warn_ratio（默认 0.8）
 *   - Token 计数：复用 commons token_standard（airy_token_counter）
 *
 * 注：为遵循「存储实现单一化」SSoT 原则并保持 daemon 数量稳定，
 * 台账实现内置于 mem_d（mem.ledger_* 命名空间），而非独立 ledger_d 进程。
 */

#ifndef AIRY_RT_MEM_LEDGER_H
#define AIRY_RT_MEM_LEDGER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mem_ledger mem_ledger_t;

/** @brief 条目类型（与设计文档 4.2 一致）。 */
typedef enum {
    LEDGER_ENTRY_SYSTEM = 0,
    LEDGER_ENTRY_TOOL_DEF,
    LEDGER_ENTRY_USER,
    LEDGER_ENTRY_TOOL_RESULT,
    LEDGER_ENTRY_ASSISTANT,
    LEDGER_ENTRY_COMPRESSED,
    LEDGER_ENTRY_CACHE_HIT
} ledger_entry_type_t;

/** @brief 条目状态。 */
typedef enum {
    LEDGER_STATUS_ACTIVE = 0,
    LEDGER_STATUS_EVICTED,
    LEDGER_STATUS_COMPRESSED,
    LEDGER_STATUS_DEDUPED
} ledger_status_t;

/** @brief 追加条目输入。 */
typedef struct {
    int entry_type;          /**< ledger_entry_type_t */
    const char *text;        /**< 条目内容（用于 token_in 估算，可 NULL） */
    size_t token_in;         /**< 显式 token_in（>0 时优先于 text 估算） */
    size_t token_out;        /**< 输出 Token（assistant 类） */
    const char *source;      /**< 写入方：gateway|think|ledger */
    const char *ref_id;      /**< 关联对象 ID（缓存条目/压缩块/工具调用） */
} ledger_entry_in_t;

/** @brief 条目查询结果。 */
typedef struct {
    uint64_t seq;
    char entry_id[33];
    char *session_id;
    int entry_type;
    size_t token_in;
    size_t token_out;
    char *source;
    int status;
    uint64_t created_at;
    char *ref_id;
} ledger_entry_view_t;

/** @brief 窗口查询结果。 */
typedef struct {
    ledger_entry_view_t *entries;
    size_t count;
    size_t total_tokens;   /**< active 条目 token_in 合计 */
    int warn;              /**< used >= budget * warn_ratio */
} ledger_window_t;

/** @brief 台账统计。 */
typedef struct {
    size_t sessions;
    size_t entries;
    size_t total_tokens;
} mem_ledger_stats_t;

/**
 * @brief 创建台账。
 * @param default_budget 默认会话预算（0 → 32768）
 * @param warn_ratio     预算告警比例（0 → 0.8）
 */
mem_ledger_t *mem_ledger_create(size_t default_budget, double warn_ratio);

/** @brief 销毁台账。 */
void mem_ledger_destroy(mem_ledger_t *ledger);

/**
 * @brief 追加条目（append-only；批量）。
 * @param out_ledger_id 返回本次追加批次首条 entry_id（调用方 AIRY_FREE）
 */
int mem_ledger_append(mem_ledger_t *ledger, const char *session_id,
                      const ledger_entry_in_t *entries, size_t count,
                      char **out_ledger_id);

/**
 * @brief 查询会话窗口（active 条目 + 总 Token + 预算告警）。
 * @param out 结果（调用方 mem_ledger_window_free 释放）
 */
int mem_ledger_window(mem_ledger_t *ledger, const char *session_id, ledger_window_t *out);
void mem_ledger_window_free(ledger_window_t *win);

/**
 * @brief 会话预算查询。
 * @param out_used/out_limit/out_headroom 输出（可 NULL）
 */
int mem_ledger_budget(mem_ledger_t *ledger, const char *session_id,
                      size_t *out_used, size_t *out_limit, size_t *out_headroom);

/**
 * @brief 标记条目状态（append-only：追加 status 变更记录）。
 * @param entry_ids 目标条目 ID 数组
 * @param count     数量
 * @param status    ledger_status_t
 * @param out_updated 实际更新数（可 NULL）
 */
int mem_ledger_mark(mem_ledger_t *ledger, const char *session_id,
                    const char **entry_ids, size_t count, int status, size_t *out_updated);

/**
 * @brief 会话历史（全量事件回放，含 status 变更记录）。
 * @param limit 最大条数（0 → 全部）
 * @param out   结果（调用方 mem_ledger_history_free 释放）
 */
int mem_ledger_history(mem_ledger_t *ledger, const char *session_id, size_t limit,
                       ledger_entry_view_t **out, size_t *out_count);
void mem_ledger_history_free(ledger_entry_view_t *items, size_t count);

/** @brief 台账统计。 */
void mem_ledger_stats(mem_ledger_t *ledger, mem_ledger_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_MEM_LEDGER_H */
