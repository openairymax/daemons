// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file test_cache.c
 * @brief Tool 缓存模块单元测试
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_cache_create_destroy(void)
{
    printf("  test_cache_create_destroy...\n");

    tool_cache_t *cache = tool_cache_create(100, 3600);
    assert(cache != NULL);

    tool_cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_cache_key_generation(void)
{
    printf("  test_cache_key_generation...\n");

    const char *tool_id = "test_tool";
    const char *params = "{\"arg\": \"value\"}";

    char *key = tool_cache_key(tool_id, params, NULL);
    assert(key != NULL);
    assert(strstr(key, tool_id) != NULL);

    /* 不同 agent 生成不同缓存键（SEC：防止跨主体缓存绕过审批） */
    char *key_a = tool_cache_key(tool_id, params, "agent_a");
    char *key_b = tool_cache_key(tool_id, params, "agent_b");
    assert(key_a != NULL && key_b != NULL);
    assert(strcmp(key_a, key_b) != 0);

    free(key);
    free(key_a);
    free(key_b);

    printf("    PASSED\n");
}

static void test_cache_key_null_inputs(void)
{
    printf("  test_cache_key_null_inputs...\n");

    char *key __attribute__((unused)) = tool_cache_key(NULL, "params", NULL);
    assert(key == NULL);

    key = tool_cache_key("tool_id", NULL, NULL);
    assert(key == NULL);

    key = tool_cache_key(NULL, NULL, NULL);
    assert(key == NULL);

    printf("    PASSED\n");
}

static void test_cache_put_get(void)
{
    printf("  test_cache_put_get...\n");

    tool_cache_t *cache = tool_cache_create(100, 3600);
    assert(cache != NULL);

    const char *key = "test_key_123";
    const char *value = "cached_result_data";

    tool_cache_put(cache, key, value);

    char *retrieved = NULL;
    int ret __attribute__((unused)) = tool_cache_get(cache, key, &retrieved);
    assert(ret == 1);
    assert(retrieved != NULL);
    assert(strcmp(retrieved, value) == 0);

    free(retrieved);
    tool_cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_cache_miss(void)
{
    printf("  test_cache_miss...\n");

    tool_cache_t *cache = tool_cache_create(100, 3600);
    assert(cache != NULL);

    char *retrieved = NULL;
    int ret __attribute__((unused)) = tool_cache_get(cache, "nonexistent_key", &retrieved);
    assert(ret == 0);

    tool_cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_cache_clear(void)
{
    printf("  test_cache_clear...\n");

    tool_cache_t *cache = tool_cache_create(100, 3600);
    assert(cache != NULL);

    tool_cache_put(cache, "key1", "value1");
    tool_cache_put(cache, "key2", "value2");
    tool_cache_put(cache, "key3", "value3");

    tool_cache_clear(cache);

    char *retrieved __attribute__((unused)) = NULL;
    assert(tool_cache_get(cache, "key1", &retrieved) == 0);
    assert(tool_cache_get(cache, "key2", &retrieved) == 0);
    assert(tool_cache_get(cache, "key3", &retrieved) == 0);

    tool_cache_destroy(cache);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Tool Cache Unit Tests\n");
    printf("=========================================\n");
    fflush(stdout);

    test_cache_create_destroy();
    test_cache_key_generation();
    test_cache_key_null_inputs();
    test_cache_put_get();
    test_cache_miss();
    test_cache_clear();

    printf("\nAll tool cache tests PASSED\n");
    return 0;
}
