// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_vocab.h
 * @brief 执行体角色词汇表 SSoT（C 侧唯一权威）。
 *
 * 角色名/别名/兜底/只读能力集合的唯一权威定义，与
 * ecosystem/agents/registry/agents.yaml 的 role 字段一一对应。
 * 编排侧（Python）经 agent.vocab A-IPC 方法运行时取值，禁止并行
 * 维护；全仓 C 源码的角色字面量只允许存在于本词汇实现内
 * （verify_release_gates.sh 门禁守护）。
 */

#ifndef AGENT_VOCAB_H
#define AGENT_VOCAB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 兜底执行体：未知角色归一化至此，保证计划总能被驱动、不因角色失配中断。 */
#define AGENT_VOCAB_FALLBACK "coding"

/* 归一化：别名 → 具体角色；未登记角色 → 兜底。
 * 返回值指向静态存储，调用方无需释放；role 为空/空串时返回兜底。 */
const char *agent_vocab_canonical(const char *role);

/* 归一化后是否只读角色（先 canonical 再判定，直接接受抽象别名输入）。 */
int agent_vocab_is_readonly(const char *role);

/* 具体执行体角色遍历。 */
size_t agent_vocab_role_count(void);
const char *agent_vocab_role_at(size_t idx);

/* 别名条目遍历：*role 返回归一化目标。 */
size_t agent_vocab_alias_count(void);
const char *agent_vocab_alias_at(size_t idx, const char **role);

/* 只读角色遍历（归一化后的具体角色集合）。 */
size_t agent_vocab_readonly_count(void);
const char *agent_vocab_readonly_at(size_t idx);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_VOCAB_H */
