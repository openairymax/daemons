// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_vocab.c
 * @brief 执行体角色词汇表 SSoT 实现：角色/别名/只读集合的唯一存放点。
 *
 * 全仓 C 源码的角色字面量收敛于此；变更本表须同步
 * ecosystem/agents/registry/agents.yaml（声明面）。
 */

#include "agent_vocab.h"

#include <string.h>

/* 具体执行体角色（与 agents.yaml enabled 执行体一致）。 */
static const char *const VOCAB_ROLES[] = {
    "product_manager", "architect", "backend",  "frontend", "devops",
    "security",        "tester",    "coding",   "data_engineer",
    "reviewer",        "analyst",
};

/* 抽象角色（规划器产出）→ 具体执行体归一化映射。 */
static const char *const VOCAB_ALIASES[][2] = {
    {"creator", "coding"},  {"generator", "coding"},
    {"writer", "coding"},   {"translator", "coding"},
    {"explainer", "coding"}, {"processor", "coding"},
    {"formatter", "coding"}, {"reactive-agent", "coding"},
    {"verifier", "tester"}, {"validator", "tester"},
    {"executor", "devops"}, {"retriever", "architect"},
    {"summarizer", "product_manager"},
};

/* 只读能力隔离集合：按归一化后角色判定（validator/verifier → tester）。 */
static const char *const VOCAB_READONLY[] = {"tester", "reviewer"};

static int vocab_streq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

const char *agent_vocab_canonical(const char *role)
{
    if (!role || role[0] == '\0')
        return AGENT_VOCAB_FALLBACK;
    for (size_t i = 0; i < sizeof(VOCAB_ROLES) / sizeof(VOCAB_ROLES[0]); i++) {
        if (vocab_streq(role, VOCAB_ROLES[i]))
            return VOCAB_ROLES[i];
    }
    for (size_t i = 0; i < sizeof(VOCAB_ALIASES) / sizeof(VOCAB_ALIASES[0]); i++) {
        if (vocab_streq(role, VOCAB_ALIASES[i][0]))
            return VOCAB_ALIASES[i][1];
    }
    return AGENT_VOCAB_FALLBACK;
}

int agent_vocab_is_readonly(const char *role)
{
    const char *canon = agent_vocab_canonical(role);
    for (size_t i = 0; i < sizeof(VOCAB_READONLY) / sizeof(VOCAB_READONLY[0]); i++) {
        if (vocab_streq(canon, VOCAB_READONLY[i]))
            return 1;
    }
    return 0;
}

size_t agent_vocab_role_count(void)
{
    return sizeof(VOCAB_ROLES) / sizeof(VOCAB_ROLES[0]);
}

const char *agent_vocab_role_at(size_t idx)
{
    return idx < agent_vocab_role_count() ? VOCAB_ROLES[idx] : NULL;
}

size_t agent_vocab_alias_count(void)
{
    return sizeof(VOCAB_ALIASES) / sizeof(VOCAB_ALIASES[0]);
}

const char *agent_vocab_alias_at(size_t idx, const char **role)
{
    if (idx >= agent_vocab_alias_count())
        return NULL;
    if (role)
        *role = VOCAB_ALIASES[idx][1];
    return VOCAB_ALIASES[idx][0];
}

size_t agent_vocab_readonly_count(void)
{
    return sizeof(VOCAB_READONLY) / sizeof(VOCAB_READONLY[0]);
}

const char *agent_vocab_readonly_at(size_t idx)
{
    return idx < agent_vocab_readonly_count() ? VOCAB_READONLY[idx] : NULL;
}
