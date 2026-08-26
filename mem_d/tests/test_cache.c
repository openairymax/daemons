// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_cache.c
 * @brief 语义缓存单元测试（13-semantic-cache-context-ledger.md §6 测试要点）
 *
 * 覆盖：缓存确定性、L0 精确命中、L1 语义阈值边界、TTL 过期、
 * LRU/容量淘汰、删除、模型隔离、命中率统计。
 */

#include "cache.h"

#include "airy_memory.h"
#include "error.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void test_create_destroy(void)
{
    printf("  test_create_destroy...\n");
    mem_cache_t *c = mem_cache_create(0, 0, 0, 0);
    assert(c != NULL);

    mem_cache_stats_t st;
    mem_cache_stats(c, &st);
    assert(st.entries == 0);
    assert(st.hits == 0 && st.misses == 0);

    mem_cache_destroy(c);
    mem_cache_destroy(NULL); /* 幂等 */
    printf("    PASSED\n");
}

/* 缓存确定性：相同请求两次调用，第一次未命中、第二次命中，响应逐字节一致 */
static void test_cache_determinism(void)
{
    printf("  test_cache_determinism...\n");
    mem_cache_t *c = mem_cache_create(64, 0, 0, 0.85);
    assert(c != NULL);

    const char *q = "how to implement a semantic cache";
    const char *resp = "{\"answer\":\"use jaccard similarity\"}";

    int hit = -1;
    double score = 0.0;
    char *cid = NULL, *out_resp = NULL;
    int ret = mem_cache_get(c, q, "gpt-4", 0, &hit, &score, &cid, &out_resp);
    assert(ret == AIRY_SUCCESS);
    assert(hit == 0);
    assert(cid == NULL && out_resp == NULL);

    char *cache_id = NULL, *exact_key = NULL;
    ret = mem_cache_put(c, q, resp, "gpt-4", 0, &cache_id, &exact_key);
    assert(ret == AIRY_SUCCESS);
    assert(cache_id != NULL && strlen(cache_id) == 32);
    assert(exact_key != NULL && strlen(exact_key) == 64);

    hit = 0;
    ret = mem_cache_get(c, q, "gpt-4", 0, &hit, &score, &cid, &out_resp);
    assert(ret == AIRY_SUCCESS);
    assert(hit == 1);
    assert(score == 1.0); /* L0 精确 */
    assert(cid != NULL && strcmp(cid, cache_id) == 0);
    assert(out_resp != NULL && strcmp(out_resp, resp) == 0);

    AIRY_FREE(cache_id);
    AIRY_FREE(exact_key);
    AIRY_FREE(cid);
    AIRY_FREE(out_resp);
    mem_cache_destroy(c);
    printf("    PASSED\n");
}

/* 模型隔离：同文本不同模型不得互命中 */
static void test_model_isolation(void)
{
    printf("  test_model_isolation...\n");
    mem_cache_t *c = mem_cache_create(64, 0, 0, 0.85);
    assert(c != NULL);

    const char *q = "explain the airymax architecture in detail";
    char *cid = NULL, *key = NULL;
    assert(mem_cache_put(c, q, "resp-a", "model-a", 0, &cid, &key) == AIRY_SUCCESS);
    AIRY_FREE(cid);
    AIRY_FREE(key);

    int hit = 1;
    char *out = NULL;
    assert(mem_cache_get(c, q, "model-b", 0, &hit, NULL, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 0); /* 精确键含 model_id，不同模型必 miss */
    assert(out == NULL);

    hit = 0;
    assert(mem_cache_get(c, q, "model-a", 0, &hit, NULL, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 1);
    AIRY_FREE(out);

    mem_cache_destroy(c);
    printf("    PASSED\n");
}

/* L1 语义命中：改写文本（高相似）命中，无关文本（低相似）未命中 */
static void test_semantic_hit_and_miss(void)
{
    printf("  test_semantic_hit_and_miss...\n");
    mem_cache_t *c = mem_cache_create(64, 0, 0, 0.85);
    assert(c != NULL);

    const char *canon = "what is the best way to cache llm responses";
    char *cid = NULL, *key = NULL;
    assert(mem_cache_put(c, canon, "resp-sem", "gpt-4", 0, &cid, &key) == AIRY_SUCCESS);
    AIRY_FREE(cid);
    AIRY_FREE(key);

    /* 高相似改写：删去高频小词 "the"（jaccard=8/9≈0.889，score≈0.906 ≥ 0.85） */
    int hit = 0;
    double score = 0.0;
    char *out = NULL;
    assert(mem_cache_get(c, "what is best way to cache llm responses", "gpt-4", 0,
                         &hit, &score, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 1);
    assert(score >= 0.85);
    assert(out != NULL && strcmp(out, "resp-sem") == 0);
    AIRY_FREE(out);

    /* 低相似：完全不同话题 */
    hit = 0;
    out = NULL;
    assert(mem_cache_get(c, "the weather in beijing today is sunny and warm", "gpt-4", 0,
                         &hit, &score, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 0);
    AIRY_FREE(out);

    /* 阈值边界：显式阈值 1.0 时改写文本不再命中（非完全相同） */
    hit = 0;
    out = NULL;
    assert(mem_cache_get(c, "what is best way to cache llm responses", "gpt-4", 1.0,
                         &hit, &score, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 0);
    AIRY_FREE(out);

    mem_cache_destroy(c);
    printf("    PASSED\n");
}

/* TTL 过期：过期条目命中即清理并 miss */
static void test_ttl_expiry(void)
{
    printf("  test_ttl_expiry...\n");
    mem_cache_t *c = mem_cache_create(64, 0, 100, 0.85); /* 默认 TTL 100ms */
    assert(c != NULL);

    const char *q = "ttl test query";
    char *cid = NULL, *key = NULL;
    assert(mem_cache_put(c, q, "resp-ttl", "gpt-4", 0, &cid, &key) == AIRY_SUCCESS);
    AIRY_FREE(cid);
    AIRY_FREE(key);

    int hit = 0;
    char *out = NULL;
    assert(mem_cache_get(c, q, "gpt-4", 0, &hit, NULL, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 1);
    AIRY_FREE(out);

    msleep(200); /* 等待过期 */

    hit = 1;
    assert(mem_cache_get(c, q, "gpt-4", 0, &hit, NULL, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 0);
    assert(out == NULL);

    mem_cache_stats_t st;
    mem_cache_stats(c, &st);
    assert(st.entries == 0); /* 过期条目已清理 */
    assert(st.evictions >= 1);

    mem_cache_destroy(c);
    printf("    PASSED\n");
}

/* LRU 淘汰：条目数上限下最旧条目被淘汰 */
static void test_lru_eviction(void)
{
    printf("  test_lru_eviction...\n");
    mem_cache_t *c = mem_cache_create(2, 0, 0, 0.85); /* 仅 2 个槽位 */
    assert(c != NULL);

    char *cid = NULL, *key = NULL;
    assert(mem_cache_put(c, "alpha one", "r1", "gpt-4", 0, &cid, &key) == AIRY_SUCCESS);
    AIRY_FREE(cid);
    AIRY_FREE(key);
    assert(mem_cache_put(c, "beta two", "r2", "gpt-4", 0, &cid, &key) == AIRY_SUCCESS);
    AIRY_FREE(cid);
    AIRY_FREE(key);
    assert(mem_cache_put(c, "gamma three", "r3", "gpt-4", 0, &cid, &key) == AIRY_SUCCESS);
    AIRY_FREE(cid);
    AIRY_FREE(key);

    /* 最旧的 alpha 被淘汰 */
    int hit = 1;
    char *out = NULL;
    assert(mem_cache_get(c, "alpha one", "gpt-4", 0, &hit, NULL, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 0);
    assert(out == NULL);

    hit = 0;
    assert(mem_cache_get(c, "beta two", "gpt-4", 0, &hit, NULL, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 1);
    AIRY_FREE(out);

    mem_cache_stats_t st;
    mem_cache_stats(c, &st);
    assert(st.entries <= 2);
    assert(st.evictions >= 1);

    mem_cache_destroy(c);
    printf("    PASSED\n");
}

/* 容量上限：字节超限触发淘汰 */
static void test_byte_capacity(void)
{
    printf("  test_byte_capacity...\n");
    /* max_bytes=16：每个条目 text+response 至少 3+3=6 字节，放 2 个后超限 */
    mem_cache_t *c = mem_cache_create(64, 16, 0, 0.85);
    assert(c != NULL);

    char *cid = NULL, *key = NULL;
    assert(mem_cache_put(c, "aa", "bb", "gpt-4", 0, &cid, &key) == AIRY_SUCCESS);
    AIRY_FREE(cid);
    AIRY_FREE(key);

    mem_cache_stats_t st;
    mem_cache_stats(c, &st);
    assert(st.entries >= 1);

    /* 连续塞入直至稳定，条目数受字节上限约束 */
    for (int i = 0; i < 20; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "q%d", i);
        char rbuf[64];
        snprintf(rbuf, sizeof(rbuf), "r%d", i);
        assert(mem_cache_put(c, buf, rbuf, "gpt-4", 0, &cid, &key) == AIRY_SUCCESS);
        AIRY_FREE(cid);
        AIRY_FREE(key);
    }
    mem_cache_stats(c, &st);
    assert(st.entries <= 4); /* 每个条目 text+response >= 4 字节 → 上限约 4 条 */

    mem_cache_destroy(c);
    printf("    PASSED\n");
}

/* 删除：指定 cache_id 删除后 miss；删除不存在条目幂等 */
static void test_delete(void)
{
    printf("  test_delete...\n");
    mem_cache_t *c = mem_cache_create(64, 0, 0, 0.85);
    assert(c != NULL);

    char *cid = NULL, *key = NULL;
    assert(mem_cache_put(c, "delete me please", "resp", "gpt-4", 0, &cid, &key) == AIRY_SUCCESS);
    assert(cid != NULL);
    AIRY_FREE(key);

    int deleted = -1;
    assert(mem_cache_del(c, cid, &deleted) == AIRY_SUCCESS);
    assert(deleted == 1);
    AIRY_FREE(cid);

    int hit = 1;
    assert(mem_cache_get(c, "delete me please", "gpt-4", 0, &hit, NULL, NULL, NULL) == AIRY_SUCCESS);
    assert(hit == 0);

    deleted = -1;
    assert(mem_cache_del(c, "00000000000000000000000000000000", &deleted) == AIRY_SUCCESS);
    assert(deleted == 0); /* 不存在幂等 */

    /* 参数校验 */
    assert(mem_cache_put(NULL, "x", "y", "m", 0, NULL, NULL) == AIRY_ERR_INVALID_PARAM);
    assert(mem_cache_get(NULL, "x", "m", 0, NULL, NULL, NULL, NULL) == AIRY_ERR_INVALID_PARAM);

    mem_cache_destroy(c);
    printf("    PASSED\n");
}

/* 命中率统计：2 次 miss + 2 次 hit → 50% */
static void test_stats(void)
{
    printf("  test_stats...\n");
    mem_cache_t *c = mem_cache_create(64, 0, 0, 0.85);
    assert(c != NULL);

    const char *q = "statistics query here";
    char *cid = NULL, *key = NULL;
    assert(mem_cache_put(c, q, "resp-st", "gpt-4", 0, &cid, &key) == AIRY_SUCCESS);
    AIRY_FREE(cid);
    AIRY_FREE(key);

    int hit = 0;
    char *out = NULL;
    assert(mem_cache_get(c, "first miss query xyz", "gpt-4", 0, &hit, NULL, NULL, NULL) == AIRY_SUCCESS);
    assert(mem_cache_get(c, "second miss query abc", "gpt-4", 0, &hit, NULL, NULL, NULL) == AIRY_SUCCESS);
    assert(mem_cache_get(c, q, "gpt-4", 0, &hit, NULL, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 1);
    AIRY_FREE(out);
    assert(mem_cache_get(c, q, "gpt-4", 0, &hit, NULL, NULL, &out) == AIRY_SUCCESS);
    assert(hit == 1);
    AIRY_FREE(out);

    mem_cache_stats_t st;
    mem_cache_stats(c, &st);
    assert(st.hits == 2);
    assert(st.misses == 2);
    assert(st.hit_rate == 0.5);

    mem_cache_destroy(c);
    printf("    PASSED\n");
}

int main(void)
{
    printf("=== Semantic Cache Unit Tests ===\n");
    test_create_destroy();
    test_cache_determinism();
    test_model_isolation();
    test_semantic_hit_and_miss();
    test_ttl_expiry();
    test_lru_eviction();
    test_byte_capacity();
    test_delete();
    test_stats();
    printf("=== All cache tests PASSED ===\n");
    return 0;
}
