// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file test_service.c
 * @brief Memory 服务单元测试
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "mem_service.h"

#include "airy_memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 测试辅助：清理 JSONL 持久化文件，确保每个测试从空状态开始 */
static void mem_test_clean_persist(void)
{
    const char *rt = getenv("AIRY_RUNTIME_DIR");
    if (rt && rt[0]) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/mem.jsonl", rt);
        remove(path);
    }
}

static void test_create_destroy(void)
{
    printf("  test_create_destroy...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(0);
    assert(svc != NULL);
    assert(mem_service_count(svc) == 0);

    mem_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_write_and_get(void)
{
    printf("  test_write_and_get...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(16);
    assert(svc != NULL);

    mem_write_request_t req = {
        .data = "hello world",
        .len = strlen("hello world"),
        .metadata = "{\"topic\":\"test\"}",
    };

    char *record_id = NULL;
    int ret = mem_service_write(svc, &req, &record_id);
    assert(ret == AIRY_SUCCESS);
    assert(record_id != NULL);
    assert(strlen(record_id) == 32);

    assert(mem_service_count(svc) == 1);

    mem_record_t rec = {0};
    ret = mem_service_get(svc, record_id, &rec);
    assert(ret == AIRY_SUCCESS);
    assert(rec.data != NULL);
    assert(rec.len == strlen("hello world"));
    assert(strncmp((const char *)rec.data, "hello world", rec.len) == 0);
    assert(rec.metadata != NULL);
    assert(strcmp(rec.metadata, "{\"topic\":\"test\"}") == 0);

    mem_record_free(&rec);
    AIRY_FREE(record_id);

    mem_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_search(void)
{
    printf("  test_search...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(32);
    assert(svc != NULL);

    /* 写入 3 条记录，含关键词 "kernel" */
    const char *datas[] = {
        "Linux kernel scheduling latency",
        "memory management in kernel space",
        "user space application logic",
    };
    const char *metas[] = {
        "{\"topic\":\"sched\"}",
        "{\"topic\":\"mm\"}",
        "{\"topic\":\"app\"}",
    };

    for (int i = 0; i < 3; i++) {
        mem_write_request_t req = {
            .data = datas[i],
            .len = strlen(datas[i]),
            .metadata = metas[i],
        };
        char *rid = NULL;
        int ret = mem_service_write(svc, &req, &rid);
        assert(ret == AIRY_SUCCESS);
        AIRY_FREE(rid);
    }

    /* 检索 "kernel" 应命中 2 条 */
    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    int ret = mem_service_search(svc, "kernel", 10, &hits, &count);
    assert(ret == AIRY_SUCCESS);
    assert(count == 2);
    for (size_t i = 0; i < count; i++) {
        assert(hits[i].record_id != NULL);
        assert(hits[i].score > 0.0f);
    }
    mem_search_hits_free(hits, count);

    /* 检索 "space" 应命中 2 条 */
    ret = mem_service_search(svc, "space", 10, &hits, &count);
    assert(ret == AIRY_SUCCESS);
    assert(count == 2);
    mem_search_hits_free(hits, count);

    /* 检索不存在的内容 */
    ret = mem_service_search(svc, "nonexistent", 10, &hits, &count);
    assert(ret == AIRY_SUCCESS);
    assert(count == 0);
    assert(hits == NULL);

    /* limit 截断 */
    ret = mem_service_search(svc, "kernel", 1, &hits, &count);
    assert(ret == AIRY_SUCCESS);
    assert(count == 1);
    mem_search_hits_free(hits, count);

    mem_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_delete(void)
{
    printf("  test_delete...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(8);
    assert(svc != NULL);

    mem_write_request_t req = {
        .data = "to be deleted",
        .len = strlen("to be deleted"),
        .metadata = NULL,
    };

    char *rid = NULL;
    int ret = mem_service_write(svc, &req, &rid);
    assert(ret == AIRY_SUCCESS);
    assert(mem_service_count(svc) == 1);

    ret = mem_service_delete(svc, rid);
    assert(ret == AIRY_SUCCESS);
    assert(mem_service_count(svc) == 0);

    /* 删除已不存在的记录应失败 */
    ret = mem_service_delete(svc, rid);
    assert(ret != AIRY_SUCCESS);

    /* get 已删除记录应失败 */
    mem_record_t rec = {0};
    ret = mem_service_get(svc, rid, &rec);
    assert(ret != AIRY_SUCCESS);

    AIRY_FREE(rid);
    mem_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_get_nonexistent(void)
{
    printf("  test_get_nonexistent...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(8);
    assert(svc != NULL);

    mem_record_t rec = {0};
    int ret = mem_service_get(svc, "nonexistent_record_id", &rec);
    assert(ret != AIRY_SUCCESS);

    mem_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_capacity_limit(void)
{
    printf("  test_capacity_limit...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(2);
    assert(svc != NULL);

    mem_write_request_t req1 = { .data = "first", .len = 5, .metadata = NULL };
    mem_write_request_t req2 = { .data = "second", .len = 6, .metadata = NULL };
    mem_write_request_t req3 = { .data = "third", .len = 5, .metadata = NULL };

    char *r1 = NULL, *r2 = NULL, *r3 = NULL;
    int wrc1 = mem_service_write(svc, &req1, &r1);
    assert(wrc1 == AIRY_SUCCESS);
    int wrc2 = mem_service_write(svc, &req2, &r2);
    assert(wrc2 == AIRY_SUCCESS);
    /* 第三条应因容量上限失败 */
    int ret = mem_service_write(svc, &req3, &r3);
    assert(ret != AIRY_SUCCESS);
    assert(r3 == NULL);

    AIRY_FREE(r1);
    AIRY_FREE(r2);

    mem_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_fill_after_delete_keeps_compact(void)
{
    printf("  test_fill_after_delete_keeps_compact...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(2);
    assert(svc != NULL);

    mem_write_request_t req1 = { .data = "first", .len = 5, .metadata = NULL };
    mem_write_request_t req2 = { .data = "second", .len = 6, .metadata = NULL };
    char *r1 = NULL, *r2 = NULL;
    int wrc1 = mem_service_write(svc, &req1, &r1);
    assert(wrc1 == AIRY_SUCCESS);
    int wrc2 = mem_service_write(svc, &req2, &r2);
    assert(wrc2 == AIRY_SUCCESS);

    /* 删除 r1，再写入新记录应成功（填补空洞或扩展） */
    int drc = mem_service_delete(svc, r1);
    assert(drc == AIRY_SUCCESS);

    mem_write_request_t req3 = { .data = "third", .len = 5, .metadata = NULL };
    char *r3 = NULL;
    int ret = mem_service_write(svc, &req3, &r3);
    assert(ret == AIRY_SUCCESS);

    /* r2 应仍可读取 */
    mem_record_t rec = {0};
    int grc = mem_service_get(svc, r2, &rec);
    assert(grc == AIRY_SUCCESS);
    mem_record_free(&rec);

    AIRY_FREE(r1);
    AIRY_FREE(r2);
    AIRY_FREE(r3);
    mem_service_destroy(svc);

    printf("    PASSED\n");
}

/* 辅助：写入一条记录并返回其 ID（调用方负责 AIRY_FREE） */
static char *mem_test_write(mem_service_t *svc, const char *data)
{
    mem_write_request_t req = { .data = data, .len = strlen(data), .metadata = NULL };
    char *rid = NULL;
    int ret = mem_service_write(svc, &req, &rid);
    assert(ret == AIRY_SUCCESS);
    assert(rid != NULL);
    return rid;
}

/* a) TF-IDF 相关度排序：语义相关的记录应排在前面 */
static void test_tfidf_relevance_ranking(void)
{
    printf("  test_tfidf_relevance_ranking...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(32);
    assert(svc != NULL);

    char *id_related = mem_test_write(svc, "agent scheduling policy for cpu");
    char *id_partial = mem_test_write(svc, "scheduler agent priority queue");
    char *id_unrelated = mem_test_write(svc, "database query optimization");

    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    /* 无关记录（无共享词项）分数为 0 被过滤，命中 2 条 */
    assert(mem_service_search(svc, "agent scheduling", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 2);
    /* 语义最相关的记录（同时命中 agent + scheduling 两个词项）排第一 */
    assert(hits[0].record_id != NULL);
    assert(strcmp(hits[0].record_id, id_related) == 0);
    assert(hits[0].score >= hits[1].score);
    assert(hits[1].record_id != NULL);
    assert(strcmp(hits[1].record_id, id_partial) == 0);
    /* 无关记录不出现在结果中 */
    for (size_t i = 0; i < count; i++)
        assert(strcmp(hits[i].record_id, id_unrelated) != 0);

    mem_search_hits_free(hits, count);
    AIRY_FREE(id_related);
    AIRY_FREE(id_partial);
    AIRY_FREE(id_unrelated);
    mem_service_destroy(svc);

    printf("    PASSED\n");
}

/* b) 无关记录排序靠后：完全无关的记录分数为 0 被过滤 */
static void test_irrelevant_ranking(void)
{
    printf("  test_irrelevant_ranking...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(32);
    assert(svc != NULL);

    char *id_pizza = mem_test_write(svc, "how to make pizza dough");
    char *id_agent = mem_test_write(svc, "agent scheduling policy for cpu");
    char *id_sched = mem_test_write(svc, "scheduler agent priority queue");

    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    /* 无关记录被过滤，仅相关记录进入结果 */
    assert(mem_service_search(svc, "agent scheduling", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 2);
    for (size_t i = 0; i < count; i++) {
        assert(hits[i].record_id != NULL);
        assert(strcmp(hits[i].record_id, id_pizza) != 0);
    }
    /* 排序：完全相关的排第一，部分相关的排第二 */
    assert(strcmp(hits[0].record_id, id_agent) == 0);
    assert(strcmp(hits[1].record_id, id_sched) == 0);
    assert(hits[0].score > hits[1].score);

    mem_search_hits_free(hits, count);
    AIRY_FREE(id_pizza);
    AIRY_FREE(id_agent);
    AIRY_FREE(id_sched);
    mem_service_destroy(svc);

    printf("    PASSED\n");
}

/* c) 混合检索降级路径：无向量可用的记录（如全停用词文本）仍能通过子串评分命中 */
static void test_mixed_fallback(void)
{
    printf("  test_mixed_fallback...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(32);
    assert(svc != NULL);

    /* 全停用词文本：tokenize 后无词项（向量为空），退化为原子串评分 */
    char *id_stop = mem_test_write(svc, "the of and to for");
    char *id_norm = mem_test_write(svc, "memory management in kernel space");

    /* 普通查询：有向量的记录经 TF-IDF 命中 */
    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    assert(mem_service_search(svc, "kernel", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 1);
    assert(hits[0].record_id != NULL);
    assert(strcmp(hits[0].record_id, id_norm) == 0);
    assert(hits[0].score > 0.0f);
    mem_search_hits_free(hits, count);

    /* 停用词查询（空向量查询）与无向量记录：走子串 fallback 仍可命中 */
    assert(mem_service_search(svc, "the", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 1);
    assert(hits[0].record_id != NULL);
    assert(strcmp(hits[0].record_id, id_stop) == 0);
    assert(hits[0].score > 0.0f);
    mem_search_hits_free(hits, count);

    AIRY_FREE(id_stop);
    AIRY_FREE(id_norm);
    mem_service_destroy(svc);

    printf("    PASSED\n");
}

/* 中文 tokenizer 验证：单字 + 相邻双字 bigram 切分，中文查询应命中相关记忆 */
static void test_chinese_search(void)
{
    printf("  test_chinese_search...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(32);
    assert(svc != NULL);

    char *id_exact = mem_test_write(svc, "内存");
    char *id_rel = mem_test_write(svc, "内存管理系统性能优化");
    char *id_unrelated = mem_test_write(svc, "网络传输协议栈实现");

    /* 查询"内存"：单字 内/存 与 bigram 内存 同时命中前两条，无关记录被过滤 */
    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    assert(mem_service_search(svc, "内存", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 2);
    assert(hits[0].record_id != NULL);
    assert(strcmp(hits[0].record_id, id_exact) == 0); /* 完全匹配的记录排第一 */
    assert(hits[0].score >= hits[1].score);
    int has_rel = 0;
    for (size_t i = 0; i < count; i++) {
        assert(strcmp(hits[i].record_id, id_unrelated) != 0);
        if (strcmp(hits[i].record_id, id_rel) == 0)
            has_rel = 1;
    }
    assert(has_rel);

    mem_search_hits_free(hits, count);
    AIRY_FREE(id_exact);
    AIRY_FREE(id_rel);
    AIRY_FREE(id_unrelated);
    mem_service_destroy(svc);

    printf("    PASSED\n");
}

/* d) 写入后检索一致性：写入立即可检索，重启后从 JSONL 重建向量检索结果不变 */
static void test_write_search_consistency(void)
{
    printf("  test_write_search_consistency...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(32);
    assert(svc != NULL);

    char *id1 = mem_test_write(svc, "memory retrieval vector index");

    /* 写入后立即可检索 */
    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    assert(mem_service_search(svc, "retrieval", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 1);
    assert(strcmp(hits[0].record_id, id1) == 0);
    mem_search_hits_free(hits, count);

    /* 再写入一条，共享词项的查询应命中两条 */
    char *id2 = mem_test_write(svc, "vector embedding model");
    assert(mem_service_search(svc, "vector", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 2);
    mem_search_hits_free(hits, count);

    /* 销毁后从 JSONL 重新加载（重启重建向量），检索结果保持一致 */
    mem_service_destroy(svc);
    svc = mem_service_create(32);
    assert(svc != NULL);
    assert(mem_service_count(svc) == 2);

    assert(mem_service_search(svc, "embedding", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 1);
    assert(strcmp(hits[0].record_id, id2) == 0);
    mem_search_hits_free(hits, count);

    assert(mem_service_search(svc, "retrieval", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 1);
    assert(strcmp(hits[0].record_id, id1) == 0);
    mem_search_hits_free(hits, count);

    assert(mem_service_search(svc, "vector", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 2);
    mem_search_hits_free(hits, count);

    mem_service_destroy(svc);
    AIRY_FREE(id1);
    AIRY_FREE(id2);

    printf("    PASSED\n");
}

/* e) embedding 后端失败自动降级：配置不可达的 embedding 端点，
 * 检索应自动降级到 TF-IDF，服务不崩溃且结果正确 */
static void test_embedding_fallback(void)
{
    printf("  test_embedding_fallback...\n");
    mem_test_clean_persist();

    setenv("AIRY_MEM_EMBEDDING_URL", "http://127.0.0.1:59999/v1", 1);
    setenv("AIRY_MEM_EMBEDDING_KEY", "sk-test", 1);

    mem_service_t *svc = mem_service_create(32);
    assert(svc != NULL);

    char *id = mem_test_write(svc, "agent scheduling policy for cpu");

    /* 写入时 embedding 调用失败（连接被拒）→ 进入冷却并降级；检索仍应命中 */
    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    assert(mem_service_search(svc, "agent scheduling", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 1);
    assert(hits[0].record_id != NULL);
    assert(strcmp(hits[0].record_id, id) == 0);
    assert(hits[0].score > 0.0f);
    mem_search_hits_free(hits, count);

    AIRY_FREE(id);
    mem_service_destroy(svc);
    unsetenv("AIRY_MEM_EMBEDDING_URL");
    unsetenv("AIRY_MEM_EMBEDDING_KEY");

    printf("    PASSED\n");
}

int main(void)
{
    /* 隔离测试的持久化目录，避免与运行中的 mem_d 或 stale 文件冲突 */
    setenv("AIRY_RUNTIME_DIR", "/tmp/agentrt_mem_test", 1);
    /* 测试默认走 TF-IDF 路径，明确关闭外部 embedding 配置干扰 */
    unsetenv("AIRY_MEM_EMBEDDING_URL");
    unsetenv("AIRY_MEM_EMBEDDING_KEY");
    mem_test_clean_persist();

    printf("=== Memory Service Unit Tests ===\n");
    test_create_destroy();
    test_write_and_get();
    test_search();
    test_delete();
    test_get_nonexistent();
    test_capacity_limit();
    test_fill_after_delete_keeps_compact();
    test_tfidf_relevance_ranking();
    test_irrelevant_ranking();
    test_mixed_fallback();
    test_chinese_search();
    test_write_search_consistency();
    test_embedding_fallback();
    printf("=== All tests PASSED ===\n");
    return 0;
}
