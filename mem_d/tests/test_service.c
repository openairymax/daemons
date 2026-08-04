// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
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

int main(void)
{
    /* 隔离测试的持久化目录，避免与运行中的 mem_d 或 stale 文件冲突 */
    setenv("AIRY_RUNTIME_DIR", "/tmp/agentrt_mem_test", 1);
    mem_test_clean_persist();

    printf("=== Memory Service Unit Tests ===\n");
    test_create_destroy();
    test_write_and_get();
    test_search();
    test_delete();
    test_get_nonexistent();
    test_capacity_limit();
    test_fill_after_delete_keeps_compact();
    printf("=== All tests PASSED ===\n");
    return 0;
}
