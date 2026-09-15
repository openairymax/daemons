// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_cache.c
 * @brief LLM 缓存模块单元测试
 */

#include "cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void test_llm_cache_create_destroy(void)
{
    printf("  test_llm_cache_create_destroy...\n");

    llm_cache_t *cache = llm_cache_create(100, 3600);
    assert(cache != NULL);

    llm_cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_llm_cache_put_get(void)
{
    printf("  test_llm_cache_put_get...\n");

    llm_cache_t *cache = llm_cache_create(100, 3600);
    assert(cache != NULL);

    const char *key = "test_key_123";
    const char *value = "test_response_content";

    llm_cache_put(cache, key, value);

    char *retrieved = NULL;
    int ret = llm_cache_get(cache, key, &retrieved);
    assert(ret == 1);
    assert(retrieved != NULL);
    assert(strcmp(retrieved, value) == 0);

    llm_cache_stats_t st;
    llm_cache_stats(cache, &st);
    assert(st.hits == 1);
    assert(st.misses == 0);
    assert(st.hit_rate == 1.0);

    free(retrieved);
    llm_cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_cache_miss(void)
{
    printf("  test_cache_miss...\n");

    llm_cache_t *cache = llm_cache_create(100, 3600);
    assert(cache != NULL);

    char *retrieved = NULL;
    int ret = llm_cache_get(cache, "nonexistent_key", &retrieved);
    assert(ret == 0);
    assert(retrieved == NULL);

    llm_cache_stats_t st;
    llm_cache_stats(cache, &st);
    assert(st.hits == 0);
    assert(st.misses == 1);
    assert(st.hit_rate == 0.0);

    llm_cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_llm_cache_clear(void)
{
    printf("  test_llm_cache_clear...\n");

    llm_cache_t *cache = llm_cache_create(100, 3600);
    assert(cache != NULL);

    llm_cache_put(cache, "key1", "value1");
    llm_cache_put(cache, "key2", "value2");
    llm_cache_put(cache, "key3", "value3");

    llm_cache_clear(cache);

    char *retrieved = NULL;
    assert(llm_cache_get(cache, "key1", &retrieved) == 0);
    assert(retrieved == NULL);
    assert(llm_cache_get(cache, "key2", &retrieved) == 0);
    assert(retrieved == NULL);
    assert(llm_cache_get(cache, "key3", &retrieved) == 0);
    assert(retrieved == NULL);

    llm_cache_stats_t st;
    llm_cache_stats(cache, &st);
    assert(st.entries == 0);
    assert(st.hits == 0);
    assert(st.misses == 3);

    llm_cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_llm_cache_size(void)
{
    printf("  test_llm_cache_size...\n");

    llm_cache_t *cache = llm_cache_create(100, 3600);
    assert(cache != NULL);
    assert(llm_cache_capacity(cache) == 100);

    llm_cache_put(cache, "key1", "value1");
    llm_cache_put(cache, "key2", "value2");

    assert(llm_cache_size(cache) == 2);

    llm_cache_stats_t st;
    llm_cache_stats(cache, &st);
    assert(st.entries == 2);
    assert(st.capacity == 100);
    assert(st.evictions == 0);

    llm_cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_llm_cache_stats(void)
{
    printf("  test_llm_cache_stats...\n");

    llm_cache_t *cache = llm_cache_create(100, 3600);
    assert(cache != NULL);

    llm_cache_put(cache, "hit1", "value1");
    llm_cache_put(cache, "hit2", "value2");

    char *retrieved = NULL;
    assert(llm_cache_get(cache, "hit1", &retrieved) == 1);
    free(retrieved);
    assert(llm_cache_get(cache, "hit2", &retrieved) == 1);
    free(retrieved);
    assert(llm_cache_get(cache, "miss1", &retrieved) == 0);
    assert(llm_cache_get(cache, "miss2", &retrieved) == 0);

    llm_cache_stats_t st;
    llm_cache_stats(cache, &st);
    assert(st.hits == 2);
    assert(st.misses == 2);
    assert(st.hit_rate == 0.5);

    llm_cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_llm_cache_eviction(void)
{
    printf("  test_llm_cache_eviction...\n");

    llm_cache_t *cache = llm_cache_create(2, 3600);
    assert(cache != NULL);

    llm_cache_put(cache, "key1", "value1");
    llm_cache_put(cache, "key2", "value2");
    llm_cache_put(cache, "key3", "value3");

    assert(llm_cache_size(cache) == 2);

    llm_cache_stats_t st;
    llm_cache_stats(cache, &st);
    assert(st.entries == 2);
    assert(st.capacity == 2);
    assert(st.evictions == 1);

    llm_cache_destroy(cache);

    printf("    PASSED\n");
}

static void test_llm_cache_ttl(void)
{
    printf("  test_llm_cache_ttl...\n");

    llm_cache_t *cache = llm_cache_create(100, 1);
    assert(cache != NULL);

    const char *key = "ttl_test_key";
    const char *value = "ttl_test_value";

    llm_cache_put(cache, key, value);

    char *retrieved = NULL;
    int ret = llm_cache_get(cache, key, &retrieved);
    (void)ret; /* suppress unused warning when -Werror */
    if (retrieved != NULL) {
        free(retrieved);
        retrieved = NULL;
    }

#ifdef _WIN32
    Sleep(2000);
#else
    /* CI-2：不能依赖单次 sleep(2)——被信号中断时 sleep() 提前返回（返回
     * 剩余秒数），偶发出现"TTL 未过期"假失败。改为按墙钟推进，循环等到
     * time(NULL) 至少前进 2 秒，消除 EINTR 抖动。 */
    {
        time_t start = time(NULL);
        while (time(NULL) - start < 2) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000}; /* 50ms */
            nanosleep(&ts, NULL);
        }
    }
#endif

    retrieved = NULL;
    ret = llm_cache_get(cache, key, &retrieved);
    assert(ret == 0);
    assert(retrieved == NULL);

    llm_cache_stats_t st;
    llm_cache_stats(cache, &st);
    assert(st.hits == 1);
    assert(st.misses == 1);
    assert(st.entries == 0);

    llm_cache_destroy(cache);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  LLM Cache Unit Tests\n");
    printf("=========================================\n");

    test_llm_cache_create_destroy();
    test_llm_cache_put_get();
    test_cache_miss();
    test_llm_cache_clear();
    test_llm_cache_size();
    test_llm_cache_stats();
    test_llm_cache_eviction();
    test_llm_cache_ttl();

    printf("\nAll LLM cache tests PASSED\n");
    return 0;
}
