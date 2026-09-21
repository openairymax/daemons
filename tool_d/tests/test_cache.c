// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_cache.c
 * @brief Tool 缓存模块单元测试
 *
 * LRU/TTL 存储已下沉 commons cache_common（表二 #1）：机制用例直接
 * 消费 cache_common 原生 API（与 tool_service 持有的同一实现）；tool
 * 专属的 key 构造用例保留。
 */

#include "cache.h"
#include "cache_common.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_cache_create_destroy(void)
{
    printf("  test_cache_create_destroy...\n");

    cache_t cache = cache_create_string_cache(100, 3600);
    assert(cache != NULL);

    cache_destroy(cache);

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

    cache_t cache = cache_create_string_cache(100, 3600);
    assert(cache != NULL);

    const char *key = "test_key_123";
    const char *value = "cached_result_data";

    cache_put_string(cache, key, value);

    char *retrieved = NULL;
    assert(cache_get_string(cache, key, &retrieved) == 1);
    assert(retrieved != NULL);
    assert(strcmp(retrieved, value) == 0);

    free(retrieved);
    cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_cache_miss(void)
{
    printf("  test_cache_miss...\n");

    cache_t cache = cache_create_string_cache(100, 3600);
    assert(cache != NULL);

    char *retrieved = NULL;
    assert(cache_get_string(cache, "nonexistent_key", &retrieved) == 0);

    cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_cache_clear(void)
{
    printf("  test_cache_clear...\n");

    cache_t cache = cache_create_string_cache(100, 3600);
    assert(cache != NULL);

    cache_put_string(cache, "key1", "value1");
    cache_put_string(cache, "key2", "value2");
    cache_put_string(cache, "key3", "value3");

    cache_clear(cache);

    char *retrieved = NULL;
    assert(cache_get_string(cache, "key1", &retrieved) == 0);
    assert(cache_get_string(cache, "key2", &retrieved) == 0);
    assert(cache_get_string(cache, "key3", &retrieved) == 0);

    cache_destroy(cache);

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
