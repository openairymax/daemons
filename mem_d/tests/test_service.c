// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service.c
 * @brief Memory 服务单元测试
 */

#include "mem_service.h"

#include "airy_memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mem_test_clean_persist(void)
{
    /* mem_d 持久路径由 AIRY_HOME 体系解析（airy_data_dir → $AIRY_HOME/data），
     * AIRY_RUNTIME_DIR 仅承载易失运行时文件，二者都清理，避免测试触碰
     * 真实生产记忆库（~/.airymaxrt/data/agentrt/memory/mem.jsonl）。 */
    const char *rt = getenv("AIRY_RUNTIME_DIR");
    if (rt && rt[0]) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/mem.jsonl", rt);
        remove(path);
    }
    const char *home = getenv("AIRY_HOME");
    if (home && home[0]) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/data/agentrt/memory/mem.jsonl", home);
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

    ret = mem_service_search(svc, "space", 10, &hits, &count);
    assert(ret == AIRY_SUCCESS);
    assert(count == 2);
    mem_search_hits_free(hits, count);

    ret = mem_service_search(svc, "nonexistent", 10, &hits, &count);
    assert(ret == AIRY_SUCCESS);
    assert(count == 0);
    assert(hits == NULL);

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

    ret = mem_service_delete(svc, rid);
    assert(ret != AIRY_SUCCESS);

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

    mem_write_request_t req1 = {.data = "first", .len = 5, .metadata = NULL};
    mem_write_request_t req2 = {.data = "second", .len = 6, .metadata = NULL};
    mem_write_request_t req3 = {.data = "third", .len = 5, .metadata = NULL};

    char *r1 = NULL, *r2 = NULL, *r3 = NULL;
    int wrc1 = mem_service_write(svc, &req1, &r1);
    assert(wrc1 == AIRY_SUCCESS);
    int wrc2 = mem_service_write(svc, &req2, &r2);
    assert(wrc2 == AIRY_SUCCESS);

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

    mem_write_request_t req1 = {.data = "first", .len = 5, .metadata = NULL};
    mem_write_request_t req2 = {.data = "second", .len = 6, .metadata = NULL};
    char *r1 = NULL, *r2 = NULL;
    int wrc1 = mem_service_write(svc, &req1, &r1);
    assert(wrc1 == AIRY_SUCCESS);
    int wrc2 = mem_service_write(svc, &req2, &r2);
    assert(wrc2 == AIRY_SUCCESS);

    int drc = mem_service_delete(svc, r1);
    assert(drc == AIRY_SUCCESS);

    mem_write_request_t req3 = {.data = "third", .len = 5, .metadata = NULL};
    char *r3 = NULL;
    int ret = mem_service_write(svc, &req3, &r3);
    assert(ret == AIRY_SUCCESS);

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

static char *mem_test_write(mem_service_t *svc, const char *data)
{
    mem_write_request_t req = {.data = data, .len = strlen(data), .metadata = NULL};
    char *rid = NULL;
    int ret = mem_service_write(svc, &req, &rid);
    assert(ret == AIRY_SUCCESS);
    assert(rid != NULL);
    return rid;
}

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

    assert(mem_service_search(svc, "agent scheduling", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 2);

    assert(hits[0].record_id != NULL);
    assert(strcmp(hits[0].record_id, id_related) == 0);
    assert(hits[0].score >= hits[1].score);
    assert(hits[1].record_id != NULL);
    assert(strcmp(hits[1].record_id, id_partial) == 0);

    for (size_t i = 0; i < count; i++)
        assert(strcmp(hits[i].record_id, id_unrelated) != 0);

    mem_search_hits_free(hits, count);
    AIRY_FREE(id_related);
    AIRY_FREE(id_partial);
    AIRY_FREE(id_unrelated);
    mem_service_destroy(svc);

    printf("    PASSED\n");
}

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

    assert(mem_service_search(svc, "agent scheduling", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 2);
    for (size_t i = 0; i < count; i++) {
        assert(hits[i].record_id != NULL);
        assert(strcmp(hits[i].record_id, id_pizza) != 0);
    }

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

static void test_mixed_fallback(void)
{
    printf("  test_mixed_fallback...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(32);
    assert(svc != NULL);

    char *id_stop = mem_test_write(svc, "the of and to for");
    char *id_norm = mem_test_write(svc, "memory management in kernel space");

    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    assert(mem_service_search(svc, "kernel", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 1);
    assert(hits[0].record_id != NULL);
    assert(strcmp(hits[0].record_id, id_norm) == 0);
    assert(hits[0].score > 0.0f);
    mem_search_hits_free(hits, count);

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

static void test_chinese_search(void)
{
    printf("  test_chinese_search...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(32);
    assert(svc != NULL);

    char *id_exact = mem_test_write(svc, "内存");
    char *id_rel = mem_test_write(svc, "内存管理系统性能优化");
    char *id_unrelated = mem_test_write(svc, "网络传输协议栈实现");

    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    assert(mem_service_search(svc, "内存", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 2);
    assert(hits[0].record_id != NULL);
    assert(strcmp(hits[0].record_id, id_exact) == 0);
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

static void test_write_search_consistency(void)
{
    printf("  test_write_search_consistency...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(32);
    assert(svc != NULL);

    char *id1 = mem_test_write(svc, "memory retrieval vector index");

    mem_search_hit_t *hits = NULL;
    size_t count = 0;
    assert(mem_service_search(svc, "retrieval", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 1);
    assert(strcmp(hits[0].record_id, id1) == 0);
    mem_search_hits_free(hits, count);

    char *id2 = mem_test_write(svc, "vector embedding model");
    assert(mem_service_search(svc, "vector", 10, &hits, &count) == AIRY_SUCCESS);
    assert(count == 2);
    mem_search_hits_free(hits, count);

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

/* KB 一等抽象：ingest → kb_search → kb_list → kb_delete 完整往返 */
static void test_kb_roundtrip(void)
{
    printf("  test_kb_roundtrip...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(64);
    assert(svc != NULL);

    const char *doc = "Linux kernel scheduling policy for CPU load balancing "
                      "across multiple cores and memory management in kernel space";
    size_t count = 0;
    int ret = mem_service_kb_ingest(svc, "kb-linux", "doc-sched", doc,
                                    strlen(doc), 32, &count);
    assert(ret == AIRY_SUCCESS);
    assert(count >= 2); /* 长文本按 32 字节分块，至少两块 */

    /* kb_search 仅命中该 KB 内的记录 */
    mem_search_hit_t *hits = NULL;
    size_t hit_count = 0;
    ret = mem_service_kb_search(svc, "kb-linux", "scheduling", 10, &hits, &hit_count);
    assert(ret == AIRY_SUCCESS);
    assert(hit_count >= 1);
    for (size_t i = 0; i < hit_count; i++) {
        assert(hits[i].record_id != NULL);
        assert(hits[i].score > 0.0f);
    }
    mem_search_hits_free(hits, hit_count);

    /* 另一个 KB 不应相互污染：在 kb-other 上搜索同样的词应无命中 */
    ret = mem_service_kb_search(svc, "kb-other", "scheduling", 10, &hits, &hit_count);
    assert(ret == AIRY_SUCCESS);
    assert(hit_count == 0);
    assert(hits == NULL);

    /* kb_list 去重列出 KB id */
    char **kb_ids = NULL;
    size_t kb_count = 0;
    ret = mem_service_kb_list(svc, &kb_ids, &kb_count);
    assert(ret == AIRY_SUCCESS);
    assert(kb_count == 1);
    assert(kb_ids != NULL);
    assert(strcmp(kb_ids[0], "kb-linux") == 0);
    mem_kb_list_free(kb_ids, kb_count);

    /* 再灌入第二个 KB，list 应为 2 */
    const char *doc2 = "network protocol stack implementation details";
    ret = mem_service_kb_ingest(svc, "kb-net", "doc-tcp", doc2, strlen(doc2), 32, &count);
    assert(ret == AIRY_SUCCESS);
    ret = mem_service_kb_list(svc, &kb_ids, &kb_count);
    assert(ret == AIRY_SUCCESS);
    assert(kb_count == 2);
    int has_linux = 0, has_net = 0;
    for (size_t i = 0; i < kb_count; i++) {
        if (strcmp(kb_ids[i], "kb-linux") == 0)
            has_linux = 1;
        if (strcmp(kb_ids[i], "kb-net") == 0)
            has_net = 1;
    }
    assert(has_linux && has_net);
    mem_kb_list_free(kb_ids, kb_count);

    /* kb_delete 只删指定 KB，另一 KB 保留 */
    size_t deleted = 0;
    ret = mem_service_kb_delete(svc, "kb-linux", &deleted);
    assert(ret == AIRY_SUCCESS);
    assert(deleted >= 1);
    assert(mem_service_kb_search(svc, "kb-linux", "scheduling", 10, &hits, &hit_count) == AIRY_SUCCESS);
    assert(hit_count == 0);
    assert(mem_service_kb_search(svc, "kb-net", "network", 10, &hits, &hit_count) == AIRY_SUCCESS);
    assert(hit_count >= 1);
    mem_search_hits_free(hits, hit_count);

    ret = mem_service_kb_delete(svc, "kb-net", &deleted);
    assert(ret == AIRY_SUCCESS);
    assert(deleted >= 1);
    assert(mem_service_count(svc) == 0);

    /* 参数校验：缺 kb_id / 空文本应拒绝 */
    assert(mem_service_kb_ingest(svc, NULL, "d", "x", 1, 0, &count) != AIRY_SUCCESS);
    assert(mem_service_kb_ingest(svc, "kb-x", "d", "x", 0, 0, &count) != AIRY_SUCCESS);

    mem_service_destroy(svc);

    printf("    PASSED\n");
}

/* UTF-8 安全分块：多字节字符不被从中截断 */
static void test_kb_utf8_chunking(void)
{
    printf("  test_kb_utf8_chunking...\n");
    mem_test_clean_persist();

    mem_service_t *svc = mem_service_create(64);
    assert(svc != NULL);

    /* 中文每字 3 字节，chunk_size=7 会切在多字节序列中间，实现必须回退 */
    const char *cn = "内存管理系统性能优化与调度";
    size_t count = 0;
    int ret = mem_service_kb_ingest(svc, "kb-cn", "doc-1", cn, strlen(cn), 7, &count);
    assert(ret == AIRY_SUCCESS);
    assert(count >= 1);

    /* 每个 chunk 都以完整 UTF-8 字符结尾：末字节不得为连续字节 0x80-0xBF */
    mem_search_hit_t *hits = NULL;
    size_t hit_count = 0;
    ret = mem_service_kb_search(svc, "kb-cn", "内存", 10, &hits, &hit_count);
    assert(ret == AIRY_SUCCESS);
    assert(hit_count >= 1);
    mem_search_hits_free(hits, hit_count);

    mem_service_destroy(svc);

    printf("    PASSED\n");
}

int main(void)
{

    setenv("AIRY_HOME", "/tmp/agentrt_mem_test_home", 1);
    setenv("AIRY_RUNTIME_DIR", "/tmp/agentrt_mem_test", 1);

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
    test_kb_roundtrip();
    test_kb_utf8_chunking();
    printf("=== All tests PASSED ===\n");
    return 0;
}
