// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_token_counter.c
 * @brief Token 估算回归测试：llm_d 层唯一可观测面 router_estimate_tokens()
 *
 * 0.1.19：token 计数下沉 commons token_standard（单一权威），本测试由
 * 已删除的 llm_d 私有 token_counter_* 对象 API 重定向到路由层估算入口，
 * 覆盖初始化/销毁、计数、空串、NULL 与单调性五条路径。
 */

#include "router/core/router_context.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_token_counter_create_destroy(void)
{
    printf("  test_token_counter_create_destroy...\n");

    assert(llm_router_init(NULL) == 0);

    const char *text = "Hello";
    assert(router_estimate_tokens(text, strlen(text)) > 0);

    llm_router_destroy();
    /* 销毁后全局上下文清零（token_cfg.model_name == NULL），
     * 回落启发式路径 prompt_len / 4。 */
    assert(router_estimate_tokens(text, 12) == 3);

    printf("    PASSED\n");
}

static void test_token_counter_count(void)
{
    printf("  test_token_counter_count...\n");

    assert(llm_router_init(NULL) == 0);

    const char *text = "Hello, world! This is a test.";
    size_t count = router_estimate_tokens(text, strlen(text));
    assert(count > 0);

    printf("    PASSED\n");
}

static void test_token_counter_empty_string(void)
{
    printf("  test_token_counter_empty_string...\n");

    assert(llm_router_init(NULL) == 0);

    assert(router_estimate_tokens("", 0) == 0);

    printf("    PASSED\n");
}

static void test_token_counter_null_input(void)
{
    printf("  test_token_counter_null_input...\n");

    assert(llm_router_init(NULL) == 0);

    /* NULL 文本走回落路径：prompt_len / 4 == 25 */
    assert(router_estimate_tokens(NULL, 100) == 25);

    printf("    PASSED\n");
}

static void test_token_counter_estimate(void)
{
    printf("  test_token_counter_estimate...\n");

    assert(llm_router_init(NULL) == 0);

    const char *short_text = "The quick brown fox.";
    const char *long_text = "The quick brown fox jumps over the lazy dog by the river.";
    size_t short_count = router_estimate_tokens(short_text, strlen(short_text));
    size_t long_count = router_estimate_tokens(long_text, strlen(long_text));
    assert(long_count > short_count);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Token Counter Unit Tests\n");
    printf("=========================================\n");

    test_token_counter_create_destroy();
    test_token_counter_count();
    test_token_counter_empty_string();
    test_token_counter_null_input();
    test_token_counter_estimate();

    llm_router_destroy();

    printf("\nAll token counter tests PASSED\n");
    return 0;
}
