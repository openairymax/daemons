// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_agent_vocab.c
 * @brief agent_vocab（执行体角色词汇表 SSoT）单元测试
 *
 * 覆盖：
 * 1. canonical：全部 13 条别名归一化、具体角色恒等、未知/空/NULL → 兜底
 * 2. is_readonly：归一化语义（validator/verifier → tester 判只读），
 *    抽象别名直传不漏网（2026-08-19 tester_v1 越界根因残余回归）
 * 3. 遍历 API：数量一致、索引越界返回 NULL、alias→role 对有效
 * 4. fallback 常量与具体角色集合一致
 */

#include "agent_vocab.h"

#include <stdio.h>
#include <string.h>

#define TEST_PASS(name) printf("✓ %s\n", name)
#define TEST_FAIL(name, reason) printf("✗ %s: %s\n", name, reason)

static int expect_canonical(const char *in, const char *want)
{
    const char *got = agent_vocab_canonical(in);
    if (got == NULL || strcmp(got, want) != 0) {
        char buf[192];
        snprintf(buf, sizeof(buf), "canonical(%s)=%s, want %s",
                 in ? in : "(null)", got ? got : "(null)", want);
        TEST_FAIL("canonical", buf);
        return -1;
    }
    return 0;
}

static int test_canonical_aliases(void)
{
    int failures = 0;
    static const char *const kAliasToCoding[] = {
        "creator", "generator", "writer",  "translator", "explainer",
        "processor", "formatter", "reactive-agent",
    };
    for (size_t i = 0; i < sizeof(kAliasToCoding) / sizeof(kAliasToCoding[0]); i++) {
        failures += (expect_canonical(kAliasToCoding[i], "coding") != 0);
    }
    failures += (expect_canonical("verifier", "tester") != 0);
    failures += (expect_canonical("validator", "tester") != 0);
    failures += (expect_canonical("executor", "devops") != 0);
    failures += (expect_canonical("retriever", "architect") != 0);
    failures += (expect_canonical("summarizer", "product_manager") != 0);
    if (failures == 0)
        TEST_PASS("canonical_aliases");
    return failures;
}

static int test_canonical_identity_and_fallback(void)
{
    static const char *const kRoles[] = {
        "product_manager", "architect", "backend",  "frontend", "devops",
        "security",        "tester",    "coding",   "data_engineer",
        "reviewer",        "analyst",
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(kRoles) / sizeof(kRoles[0]); i++) {
        failures += (expect_canonical(kRoles[i], kRoles[i]) != 0);
    }
    failures += (expect_canonical("no-such-role", AGENT_VOCAB_FALLBACK) != 0);
    failures += (expect_canonical("", AGENT_VOCAB_FALLBACK) != 0);
    failures += (expect_canonical(NULL, AGENT_VOCAB_FALLBACK) != 0);
    if (failures == 0)
        TEST_PASS("canonical_identity_and_fallback");
    return failures;
}

static int expect_readonly(const char *in, int want)
{
    int got = agent_vocab_is_readonly(in);
    if (got != want) {
        char buf[160];
        snprintf(buf, sizeof(buf), "is_readonly(%s)=%d, want %d",
                 in ? in : "(null)", got, want);
        TEST_FAIL("is_readonly", buf);
        return -1;
    }
    return 0;
}

static int test_is_readonly_normalized(void)
{
    int failures = 0;
    failures += (expect_readonly("tester", 1) != 0);
    failures += (expect_readonly("reviewer", 1) != 0);
    /* 归一化语义：抽象别名直传必须命中只读（漏网回归）。 */
    failures += (expect_readonly("validator", 1) != 0);
    failures += (expect_readonly("verifier", 1) != 0);
    failures += (expect_readonly("coding", 0) != 0);
    failures += (expect_readonly("backend", 0) != 0);
    failures += (expect_readonly("creator", 0) != 0);
    failures += (expect_readonly("unknown-x", 0) != 0);
    failures += (expect_readonly(NULL, 0) != 0);
    failures += (expect_readonly("", 0) != 0);
    if (failures == 0)
        TEST_PASS("is_readonly_normalized");
    return failures;
}

static int test_traversals(void)
{
    int failures = 0;
    size_t role_n = agent_vocab_role_count();
    if (role_n != 11) {
        char buf[96];
        snprintf(buf, sizeof(buf), "role_count=%zu, want 11", role_n);
        TEST_FAIL("traversals", buf);
        failures++;
    } else {
        for (size_t i = 0; i < role_n; i++) {
            if (agent_vocab_role_at(i) == NULL || agent_vocab_role_at(i)[0] == '\0') {
                TEST_FAIL("traversals", "role_at returned empty");
                failures++;
                break;
            }
        }
    }
    if (agent_vocab_role_at(role_n) != NULL) {
        TEST_FAIL("traversals", "role_at out-of-range should be NULL");
        failures++;
    }

    size_t alias_n = agent_vocab_alias_count();
    if (alias_n != 13) {
        char buf[96];
        snprintf(buf, sizeof(buf), "alias_count=%zu, want 13", alias_n);
        TEST_FAIL("traversals", buf);
        failures++;
    } else {
        for (size_t i = 0; i < alias_n; i++) {
            const char *target = NULL;
            const char *alias = agent_vocab_alias_at(i, &target);
            if (alias == NULL || target == NULL || alias[0] == '\0' ||
                target[0] == '\0') {
                TEST_FAIL("traversals", "alias_at returned invalid pair");
                failures++;
                break;
            }
        }
    }
    if (agent_vocab_alias_at(alias_n, NULL) != NULL) {
        TEST_FAIL("traversals", "alias_at out-of-range should be NULL");
        failures++;
    }

    size_t ro_n = agent_vocab_readonly_count();
    if (ro_n != 2) {
        char buf[96];
        snprintf(buf, sizeof(buf), "readonly_count=%zu, want 2", ro_n);
        TEST_FAIL("traversals", buf);
        failures++;
    } else {
        int has_tester = 0;
        int has_reviewer = 0;
        for (size_t i = 0; i < ro_n; i++) {
            const char *r = agent_vocab_readonly_at(i);
            if (r && strcmp(r, "tester") == 0)
                has_tester = 1;
            if (r && strcmp(r, "reviewer") == 0)
                has_reviewer = 1;
        }
        if (!has_tester || !has_reviewer) {
            TEST_FAIL("traversals", "readonly set must be {tester, reviewer}");
            failures++;
        }
    }
    if (agent_vocab_readonly_at(ro_n) != NULL) {
        TEST_FAIL("traversals", "readonly_at out-of-range should be NULL");
        failures++;
    }

    if (failures == 0)
        TEST_PASS("traversals");
    return failures;
}

static int test_fallback_in_roles(void)
{
    for (size_t i = 0; i < agent_vocab_role_count(); i++) {
        if (strcmp(agent_vocab_role_at(i), AGENT_VOCAB_FALLBACK) == 0) {
            TEST_PASS("fallback_in_roles");
            return 0;
        }
    }
    TEST_FAIL("fallback_in_roles", "fallback must be one of concrete roles");
    return -1;
}

int main(void)
{
    int failures = 0;
    failures += (test_canonical_aliases() != 0);
    failures += (test_canonical_identity_and_fallback() != 0);
    failures += (test_is_readonly_normalized() != 0);
    failures += (test_traversals() != 0);
    failures += (test_fallback_in_roles() != 0);

    if (failures == 0) {
        printf("\nAll agent_vocab tests passed\n");
        return 0;
    }
    printf("\n%d test(s) failed\n", failures);
    return 1;
}
