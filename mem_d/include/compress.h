/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file compress.h
 * @brief 提示词压缩（Prompt Compression，mem.compress 命名空间）。
 *
 * 实现 AirymaxRT 14-prompt-compression.md 分层级联（L1 → L2）：
 *   - L1 规则裁剪（默认开）：tool_result 超长截断、超轮数丢最早轮、精确去重
 *   - L2 抽取式摘要（默认开）：历史轮次抽取关键句（位置加权 + 高频术语 + 指令动词）
 *   - 保护规则：system / tool_def / 当前请求（最后一条 user）永不压缩
 *   - 与台账联动：plan 返回被压缩条目，调用方 ledger.mark(COMPRESSED) +
 *     ledger.append(compressed 块)，全程可回放（14-prompt-compression.md §4.4）
 *
 * 契约（13-semantic-cache-context-ledger.md §4.5）：台账只提供候选与依据
 * （entry_type/status/token_in/预算），条目原文由调用方经 entries[] 传入；
 * 压缩计划据此生成，不直接读写台账条目文本。
 */

#ifndef AIRY_RT_MEM_COMPRESS_H
#define AIRY_RT_MEM_COMPRESS_H

#include "ledger.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COMPRESS_DEFAULT_MAX_TOOL_TOKENS 1024
#define COMPRESS_DEFAULT_MAX_TURNS 20

/** @brief 单条目压缩动作。 */
typedef enum {
    COMPRESS_ACTION_NONE = 0, /**< 不压缩（保护条目 / 无需处理） */
    COMPRESS_ACTION_TRUNCATE, /**< L1：tool_result 超长截断 */
    COMPRESS_ACTION_EXTRACT,  /**< L2：抽取式摘要替换原文 */
    COMPRESS_ACTION_DROP,     /**< L1：超轮数丢弃最早轮 */
    COMPRESS_ACTION_DEDUP     /**< L1：文本精确哈希去重（保留首条） */
} compress_action_t;

/** @brief 压缩配置。 */
typedef struct {
    size_t max_tool_tokens; /**< L1：tool_result token 上限（默认 1024） */
    size_t max_turns;       /**< L1：保留最大轮数（默认 20） */
    int l1_enabled;         /**< L1 开关（默认开） */
    int l2_enabled;         /**< L2 开关（默认开） */
    int dedup;              /**< 去重开关（默认开） */
} compress_config_t;

/** @brief 压缩候选条目（调用方提供原文）。 */
typedef struct {
    const char *entry_id;   /**< 台账条目 ID（供 mark 回指） */
    int entry_type;         /**< ledger_entry_type_t */
    const char *text;       /**< 条目原文（可为 NULL） */
    size_t token_in;        /**< 显式 token 数（0 → 用 token_standard 估算） */
} compress_entry_in_t;

/** @brief 压缩计划条目。 */
typedef struct {
    const char *entry_id;    /**< 被压缩条目 ID（调用方据此 ledger.mark） */
    int entry_type;          /**< ledger_entry_type_t */
    int action;              /**< compress_action_t */
} compress_plan_item_t;

/**
 * @brief 生成压缩计划并输出压缩后重组上下文。
 *
 * 保护规则（14-prompt-compression.md §2）：system / tool_def /
 * 当前请求（最后一条 user）永不压缩。
 *
 * @param ledger          台账（用于预算/轮次依据；可为 NULL 时预算视为 0）
 * @param session_id      会话 ID（预算查询用，可为 NULL）
 * @param entries         窗口条目原文（active 条目，按顺序）
 * @param count           条目数
 * @param cfg             压缩配置（NULL → 默认）
 * @param out_context     压缩后重组上下文文本（调用方 mem_compress_plan_free）
 * @param out_saved_tokens 节省 token 数（token_standard 计数）
 * @param out_actions     压缩计划条目数组（调用方 mem_compress_plan_free）
 * @param out_action_count 计划条目数（0 = 无需压缩）
 * @return AIRY_SUCCESS / AIRY_ERR_*
 */
int mem_compress_plan(mem_ledger_t *ledger, const char *session_id,
                      const compress_entry_in_t *entries, size_t count,
                      const compress_config_t *cfg,
                      char **out_context, size_t *out_saved_tokens,
                      compress_plan_item_t **out_actions, size_t *out_action_count);

/** @brief 释放 plan 输出（out_context 与 out_actions）。 */
void mem_compress_plan_free(char *context, compress_plan_item_t *actions, size_t action_count);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_MEM_COMPRESS_H */
