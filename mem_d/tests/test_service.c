// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service.c
 * @brief Memory 服务单元测试
 */

#include "core/mem_service.h"

#include "airy_memory.h"
#include "safe_utf8.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    assert(mem_service_write(svc, &req1, &r1) == AIRY_SUCCESS);
    assert(mem_service_write(svc, &req2, &r2) == AIRY_SUCCESS);

    /* 满则淘汰最旧、保留最新：写入不再失败，历史窗口随轮次滚动。 */
    assert(mem_service_write(svc, &req3, &r3) == AIRY_SUCCESS);
    assert(mem_service_count(svc) == 2);

    mem_record_t got;
    AIRY_MEMSET(&got, 0, sizeof(got));
    assert(mem_service_get(svc, r1, &got) != AIRY_SUCCESS);
    assert(mem_service_get(svc, r2, &got) == AIRY_SUCCESS);
    mem_record_free(&got);
    assert(mem_service_get(svc, r3, &got) == AIRY_SUCCESS);
    mem_record_free(&got);

    AIRY_FREE(r1);
    AIRY_FREE(r2);
    AIRY_FREE(r3);

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

/* 存量数据修复：早期版本按字节下标截断写坏的记忆记录，落盘后含有被腰斩
 * 的多字节 UTF-8 序列；这类记录被召回并随请求体发给模型服务时会被拒绝
 * （HTTP 400 invalid unicode code point），用户侧却只看到连通性故障。
 * 载入时必须一次性清洗，且先备份原文件。 */
static void test_persist_utf8_repair(void)
{
    printf("  test_persist_utf8_repair...\n");
    mem_test_clean_persist();

    /* 先建一次服务，确保 data/agentrt/memory 目录已创建 */
    mem_service_t *boot = mem_service_create(16);
    assert(boot != NULL);
    mem_service_destroy(boot);

    const char *home = getenv("AIRY_HOME");
    assert(home != NULL && home[0] != '\0');
    char path[4096];
    char bak[4096];
    snprintf(path, sizeof(path), "%s/data/agentrt/memory/mem.jsonl", home);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    remove(bak);

    /* 构造一条被字节截断的中文记录：data 尾部是被腰斩的 3 字节序列 */
    const char *head = "{\"record_id\":\"0123456789abcdef0123456789abcdef\",\"data\":\"hi";
    const unsigned char tail[2] = {0xE4, 0xB8};
    char line[512];
    int n = snprintf(line, sizeof(line),
                     "%s%c%c\",\"metadata\":null,\"created_at\":1700000000,\"len\":5}\n",
                     head, (char)tail[0], (char)tail[1]);
    assert(n > 0 && (size_t)n < sizeof(line));

    FILE *w = fopen(path, "wb");
    assert(w != NULL);
    assert(fwrite(line, 1, (size_t)n, w) == (size_t)n);
    fclose(w);

    /* 载入触发一次性清洗 */
    mem_service_t *svc = mem_service_create(16);
    assert(svc != NULL);
    assert(mem_service_count(svc) == 1);

    /* 原文件被完整备份（含原始非法字节） */
    FILE *rb = fopen(bak, "rb");
    assert(rb != NULL);
    char raw[1024];
    size_t raw_len = fread(raw, 1, sizeof(raw), rb);
    fclose(rb);
    assert(raw_len == (size_t)n);
    assert(memcmp(raw, line, (size_t)n) == 0);

    /* 主文件已被清洗为合法 UTF-8 */
    FILE *rm = fopen(path, "rb");
    assert(rm != NULL);
    char cleaned[1024];
    size_t clean_len = fread(cleaned, 1, sizeof(cleaned) - 1, rm);
    fclose(rm);
    cleaned[clean_len] = '\0';
    assert(clean_len > 0);
    assert(utf8_validate(cleaned, clean_len));

    /* 召回内容同样为合法 UTF-8 */
    mem_record_t rec = {0};
    int ret = mem_service_get(svc, "0123456789abcdef0123456789abcdef", &rec);
    assert(ret == AIRY_SUCCESS);
    assert(rec.data != NULL);
    assert(utf8_validate((const char *)rec.data, rec.len));
    mem_record_free(&rec);

    mem_service_destroy(svc);

    remove(bak);
    mem_test_clean_persist();

    printf("    PASSED\n");
}

/* 压缩后磁盘重载契约：删除/KB 删除触发 mem_persist_rewrite_all 之后，
 * 新一轮 mem_persist_load_existing 必须还原重写后的状态——被删记录
 * 不复活、保留记录内容往返一致。 */
static void test_reload_after_rewrite(void)
{
    printf("  test_reload_after_rewrite...\n");
    mem_test_clean_persist();

    const char *texts[3] = {"alpha record", "beta record", "gamma record"};
    char *ids[3] = {NULL, NULL, NULL};

    mem_service_t *svc = mem_service_create(16);
    assert(svc != NULL);
    for (int i = 0; i < 3; i++) {
        mem_write_request_t req = {
            .data = (void *)texts[i],
            .len = strlen(texts[i]),
            .metadata = NULL,
        };
        int ret = mem_service_write(svc, &req, &ids[i]);
        assert(ret == AIRY_SUCCESS && ids[i] != NULL);
    }
    mem_service_destroy(svc);

    svc = mem_service_create(16);
    assert(svc != NULL);
    assert(mem_service_count(svc) == 3);

    /* 删除中间一条：触发 rewrite_all 落盘 */
    int ret = mem_service_delete(svc, ids[1]);
    assert(ret == AIRY_SUCCESS);
    assert(mem_service_count(svc) == 2);
    mem_service_destroy(svc);

    /* 重载：被删记录不复活，保留记录内容一致 */
    svc = mem_service_create(16);
    assert(svc != NULL);
    assert(mem_service_count(svc) == 2);

    mem_record_t rec = {0};
    assert(mem_service_get(svc, ids[1], &rec) == AIRY_ERR_NOT_FOUND);
    for (int i = 0; i < 3; i += 2) {
        assert(mem_service_get(svc, ids[i], &rec) == AIRY_SUCCESS);
        assert(rec.len == strlen(texts[i]));
        assert(strncmp((const char *)rec.data, texts[i], rec.len) == 0);
        mem_record_free(&rec);
    }

    for (int i = 0; i < 3; i++)
        AIRY_FREE(ids[i]);
    mem_service_destroy(svc);
    mem_test_clean_persist();

    printf("    PASSED\n");
}

static char *write_text(mem_service_t *svc, const char *text)
{
    mem_write_request_t req = {
        .data = (void *)text,
        .len = strlen(text),
        .metadata = NULL,
    };
    char *id = NULL;
    assert(mem_service_write(svc, &req, &id) == AIRY_SUCCESS);
    assert(id != NULL);
    return id;
}

static char *write_meta(mem_service_t *svc, const char *text, const char *meta)
{
    mem_write_request_t req = {
        .data = (void *)text,
        .len = strlen(text),
        .metadata = meta,
    };
    char *id = NULL;
    assert(mem_service_write(svc, &req, &id) == AIRY_SUCCESS);
    assert(id != NULL);
    return id;
}

static size_t recent_ids(mem_service_t *svc, char *out[], size_t cap)
{
    mem_recent_item_t *items = NULL;
    size_t n = 0;
    assert(mem_service_recent(svc, 0, &items, &n) == AIRY_SUCCESS);
    assert(n <= cap);
    for (size_t i = 0; i < n; i++)
        out[i] = items[i].record_id ? AIRY_STRDUP(items[i].record_id) : NULL;
    mem_recent_items_free(items, n);
    return n;
}

static void ids_free(char *ids[], size_t n)
{
    for (size_t i = 0; i < n; i++)
        AIRY_FREE(ids[i]);
}

/* C-28 回归①：同秒三条写入，recent 必须是写入序倒序（不依赖秒级时间戳）。 */
static void test_recent_order_same_second(void)
{
    printf("  test_recent_order_same_second...\n");
    mem_test_clean_persist();
    mem_service_t *svc = mem_service_create(16);
    assert(svc != NULL);

    char *ids[3];
    ids[0] = write_text(svc, "first record");
    ids[1] = write_text(svc, "second record");
    ids[2] = write_text(svc, "third record");

    char *got[4] = {NULL};
    size_t n = recent_ids(svc, got, 4);
    assert(n == 3);
    assert(strcmp(got[0], ids[2]) == 0);
    assert(strcmp(got[1], ids[1]) == 0);
    assert(strcmp(got[2], ids[0]) == 0);

    ids_free(got, n);
    ids_free(ids, 3);
    mem_service_destroy(svc);
    mem_test_clean_persist();
    printf("    PASSED\n");
}

/* C-28 回归②：删除首条（最旧）后，尾部记录不得被搬入 0 号位并排到队首。
 * 这是交换删除的原始反例：写 A/B/C 删 A，错误实现返回 B/C。 */
static void test_recent_order_delete_head(void)
{
    printf("  test_recent_order_delete_head...\n");
    mem_test_clean_persist();
    mem_service_t *svc = mem_service_create(16);
    assert(svc != NULL);

    char *ids[3];
    ids[0] = write_text(svc, "alpha record");
    ids[1] = write_text(svc, "beta record");
    ids[2] = write_text(svc, "gamma record");
    assert(mem_service_delete(svc, ids[0]) == AIRY_SUCCESS);

    char *got[4] = {NULL};
    size_t n = recent_ids(svc, got, 4);
    assert(n == 2);
    assert(strcmp(got[0], ids[2]) == 0);
    assert(strcmp(got[1], ids[1]) == 0);

    ids_free(got, n);
    ids_free(ids, 3);
    mem_service_destroy(svc);
    mem_test_clean_persist();
    printf("    PASSED\n");
}

/* C-28 回归③：四条中删第二条，剩余顺序须为写入序（交换删除会得到 D/C/A）。 */
static void test_recent_order_delete_middle(void)
{
    printf("  test_recent_order_delete_middle...\n");
    mem_test_clean_persist();
    mem_service_t *svc = mem_service_create(16);
    assert(svc != NULL);

    char *ids[4];
    ids[0] = write_text(svc, "one record");
    ids[1] = write_text(svc, "two record");
    ids[2] = write_text(svc, "three record");
    ids[3] = write_text(svc, "four record");
    assert(mem_service_delete(svc, ids[1]) == AIRY_SUCCESS);

    char *got[5] = {NULL};
    size_t n = recent_ids(svc, got, 5);
    assert(n == 3);
    assert(strcmp(got[0], ids[3]) == 0);
    assert(strcmp(got[1], ids[2]) == 0);
    assert(strcmp(got[2], ids[0]) == 0);

    ids_free(got, n);
    ids_free(ids, 4);
    mem_service_destroy(svc);
    mem_test_clean_persist();
    printf("    PASSED\n");
}

/* C-28 回归④：删除末尾一条不触发搬移，顺序同样成立。 */
static void test_recent_order_delete_tail(void)
{
    printf("  test_recent_order_delete_tail...\n");
    mem_test_clean_persist();
    mem_service_t *svc = mem_service_create(16);
    assert(svc != NULL);

    char *ids[3];
    ids[0] = write_text(svc, "one record");
    ids[1] = write_text(svc, "two record");
    ids[2] = write_text(svc, "three record");
    assert(mem_service_delete(svc, ids[2]) == AIRY_SUCCESS);

    char *got[4] = {NULL};
    size_t n = recent_ids(svc, got, 4);
    assert(n == 2);
    assert(strcmp(got[0], ids[1]) == 0);
    assert(strcmp(got[1], ids[0]) == 0);

    ids_free(got, n);
    ids_free(ids, 3);
    mem_service_destroy(svc);
    mem_test_clean_persist();
    printf("    PASSED\n");
}

/* C-28 回归⑤：删空后重新写入，recent 只含新记录且下标从 0 重新起算。 */
static void test_recent_order_delete_all(void)
{
    printf("  test_recent_order_delete_all...\n");
    mem_test_clean_persist();
    mem_service_t *svc = mem_service_create(16);
    assert(svc != NULL);

    char *ids[3];
    ids[0] = write_text(svc, "old a record");
    ids[1] = write_text(svc, "old b record");
    ids[2] = write_text(svc, "old c record");
    for (int i = 0; i < 3; i++)
        assert(mem_service_delete(svc, ids[i]) == AIRY_SUCCESS);
    assert(mem_service_count(svc) == 0);

    char *fresh = write_text(svc, "fresh record");
    char *got[4] = {NULL};
    size_t n = recent_ids(svc, got, 4);
    assert(n == 1);
    assert(strcmp(got[0], fresh) == 0);

    ids_free(got, n);
    AIRY_FREE(fresh);
    ids_free(ids, 3);
    mem_service_destroy(svc);
    mem_test_clean_persist();
    printf("    PASSED\n");
}

/* C-28 回归⑥：重写落盘后重载，写入序不变量随磁盘顺序一起还原。 */
static void test_recent_order_after_reload(void)
{
    printf("  test_recent_order_after_reload...\n");
    mem_test_clean_persist();

    char *ids[3];
    mem_service_t *svc = mem_service_create(16);
    assert(svc != NULL);
    ids[0] = write_text(svc, "disk a record");
    ids[1] = write_text(svc, "disk b record");
    ids[2] = write_text(svc, "disk c record");
    assert(mem_service_delete(svc, ids[1]) == AIRY_SUCCESS);
    mem_service_destroy(svc);

    svc = mem_service_create(16);
    assert(svc != NULL);
    char *got[4] = {NULL};
    size_t n = recent_ids(svc, got, 4);
    assert(n == 2);
    assert(strcmp(got[0], ids[2]) == 0);
    assert(strcmp(got[1], ids[0]) == 0);

    ids_free(got, n);
    ids_free(ids, 3);
    mem_service_destroy(svc);
    mem_test_clean_persist();
    printf("    PASSED\n");
}

/* S-P1-16：按 metadata.conv 过滤，且过滤先于截断——旧会话记录再新也不能
 * 占满 limit 把本会话挤空；检索同样只在会话内打分。 */
static void test_recent_conv_scope(void)
{
    printf("  test_recent_conv_scope...\n");
    mem_test_clean_persist();
    mem_service_t *svc = mem_service_create(16);
    assert(svc != NULL);

    char *a1 = write_meta(svc, "alpha record one", "{\"conv\":\"sess_a\"}");
    char *b1 = write_meta(svc, "beta record one", "{\"conv\":\"sess_b\"}");
    char *a2 = write_meta(svc, "alpha record two", "{\"conv\":\"sess_a\"}");

    mem_recent_item_t *items = NULL;
    size_t n = 0;
    assert(mem_recent_scoped(svc, 1, "sess_a", &items, &n) == AIRY_SUCCESS);
    assert(n == 1);
    assert(strcmp(items[0].record_id, a2) == 0);
    mem_recent_items_free(items, n);

    items = NULL;
    n = 99;
    assert(mem_recent_scoped(svc, 5, "sess_c", &items, &n) == AIRY_SUCCESS);
    assert(n == 0);
    assert(items == NULL);

    mem_search_hit_t *hits = NULL;
    size_t hit_n = 0;
    assert(mem_search_scoped(svc, NULL, "sess_a", "record", 5, &hits, &hit_n) == AIRY_SUCCESS);
    assert(hit_n == 2);
    mem_search_hits_free(hits, hit_n);

    hits = NULL;
    hit_n = 0;
    assert(mem_search_scoped(svc, NULL, NULL, "record", 5, &hits, &hit_n) == AIRY_SUCCESS);
    assert(hit_n == 3);
    mem_search_hits_free(hits, hit_n);

    AIRY_FREE(a1);
    AIRY_FREE(b1);
    AIRY_FREE(a2);
    mem_service_destroy(svc);
    mem_test_clean_persist();
    printf("    PASSED\n");
}

int main(void)
{

    /* CI-2：PID 隔离，避免 ctest -j 并行互踩 */
    char home[256];
    char runtime_dir[256];
    snprintf(home, sizeof(home), "/tmp/agentrt_mem_test_home_%d", (int)getpid());
    snprintf(runtime_dir, sizeof(runtime_dir), "/tmp/agentrt_mem_test_%d", (int)getpid());
    setenv("AIRY_HOME", home, 1);
    setenv("AIRY_RUNTIME_DIR", runtime_dir, 1);

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
    test_persist_utf8_repair();
    test_reload_after_rewrite();
    test_recent_order_same_second();
    test_recent_order_delete_head();
    test_recent_order_delete_middle();
    test_recent_order_delete_tail();
    test_recent_order_delete_all();
    test_recent_order_after_reload();
    test_recent_conv_scope();
    printf("=== All tests PASSED ===\n");
    return 0;
}
