// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service.c
 * @brief Tool 服务核心功能单元测试
 */

#include "tool_service.h"
#include "tool_builtin_internal.h"

#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Release 构建（-DNDEBUG）会编译掉 CHECK()，导致校验形同虚设且
 * CHECK(expr) 内副作用（如 mkdir/fopen 设置操作）丢失。
 * CHECK 在 NDEBUG 下同样失败退出，测试在任意构建类型下都有效。 */
#ifndef CHECK
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #cond);                                      \
            abort();                                                       \
        }                                                                  \
    } while (0)
#endif

static void test_service_create_destroy(void)
{
    printf("  test_service_create_destroy...\n");

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);

    tool_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_service_register_tool(void)
{
    printf("  test_service_register_tool...\n");

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "test_tool";
    meta.name = "test_tool";
    meta.description = "A test tool";
    meta.executable = "/bin/echo";
    meta.timeout_sec = 10;

    int ret __attribute__((unused)) = tool_service_register(svc, &meta);
    CHECK(ret == 0);

    tool_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_service_list_tools(void)
{
    printf("  test_service_list_tools...\n");

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);

    tool_metadata_t meta1;
    AIRY_MEMSET(&meta1, 0, sizeof(meta1));
    meta1.id = "tool1";
    meta1.name = "tool1";
    meta1.executable = "/bin/echo";
    meta1.timeout_sec = 10;

    tool_metadata_t meta2;
    AIRY_MEMSET(&meta2, 0, sizeof(meta2));
    meta2.id = "tool2";
    meta2.name = "tool2";
    meta2.executable = "/bin/cat";
    meta2.timeout_sec = 10;

    tool_service_register(svc, &meta1);
    tool_service_register(svc, &meta2);

    char *tools_json = tool_service_list(svc);
    CHECK(tools_json != NULL);
    printf("    Tools: %s\n", tools_json);
    free(tools_json);

    tool_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_service_get_tool(void)
{
    printf("  test_service_get_tool...\n");

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "get_test_tool";
    meta.name = "get_test_tool";
    meta.executable = "/bin/echo";
    meta.timeout_sec = 10;

    tool_service_register(svc, &meta);

    tool_metadata_t *found __attribute__((unused)) = tool_service_get(svc, "get_test_tool");
    CHECK(found != NULL);
    CHECK(strcmp(found->name, "get_test_tool") == 0);
    tool_metadata_free(found);

    tool_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_service_unregister_tool(void)
{
    printf("  test_service_unregister_tool...\n");

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "unregister_test";
    meta.name = "unregister_test";
    meta.executable = "/bin/echo";
    meta.timeout_sec = 10;

    tool_service_register(svc, &meta);

    int ret __attribute__((unused)) = tool_service_unregister(svc, "unregister_test");
    CHECK(ret == 0);

    tool_metadata_t *found __attribute__((unused)) = tool_service_get(svc, "unregister_test");
    CHECK(found == NULL);

    tool_service_destroy(svc);

    printf("    PASSED\n");
}

#ifndef _WIN32
static void test_fs_delete_builtin(void)
{
    printf("  test_fs_delete_builtin...\n");

    const char *fpath = "/tmp/airy_test_fs_delete_file.txt";
    const char *dpath = "/tmp/airy_test_fs_delete_dir";
    char jbuf[512];

    /* 先清理上次运行可能残留的状态，避免 mkdir EEXIST / 非空目录误判。 */
    unlink(fpath);
    tool_result_t cleanup = {0};
    (void)fs_delete_tool("{\"path\":\"/tmp/airy_test_fs_delete_dir\",\"recursive\":true}",
                         &cleanup);
    AIRY_FREE(cleanup.output);
    AIRY_FREE(cleanup.error);

    FILE *fp = fopen(fpath, "wb");
    CHECK(fp != NULL);
    fputs("x", fp);
    fclose(fp);
    CHECK(mkdir(dpath, 0755) == 0);

    /* 1. 删除文件 */
    tool_result_t *res = (tool_result_t *)calloc(1, sizeof(tool_result_t));
    CHECK(res != NULL);
    snprintf(jbuf, sizeof(jbuf), "{\"path\":\"%s\"}", fpath);
    int rc = fs_delete_tool(jbuf, res);
    CHECK(rc == 0 && res->success == 1);
    CHECK(access(fpath, F_OK) != 0);
    tool_result_free(res);

    /* 2. 删除空目录 */
    res = (tool_result_t *)calloc(1, sizeof(tool_result_t));
    CHECK(res != NULL);
    snprintf(jbuf, sizeof(jbuf), "{\"path\":\"%s\"}", dpath);
    rc = fs_delete_tool(jbuf, res);
    CHECK(rc == 0 && res->success == 1);
    CHECK(access(dpath, F_OK) != 0);
    tool_result_free(res);

    /* 3. 非空目录非递归 → 拒绝且目录保留 */
    CHECK(mkdir(dpath, 0755) == 0);
    FILE *sub = fopen("/tmp/airy_test_fs_delete_dir/sub.txt", "wb");
    CHECK(sub != NULL);
    fputs("x", sub);
    fclose(sub);
    res = (tool_result_t *)calloc(1, sizeof(tool_result_t));
    CHECK(res != NULL);
    snprintf(jbuf, sizeof(jbuf), "{\"path\":\"%s\"}", dpath);
    rc = fs_delete_tool(jbuf, res);
    CHECK(rc == 0 && res->success == 0);
    CHECK(access(dpath, F_OK) == 0);
    tool_result_free(res);

    /* 4. 非空目录 recursive=1 → 递归删除 */
    res = (tool_result_t *)calloc(1, sizeof(tool_result_t));
    CHECK(res != NULL);
    snprintf(jbuf, sizeof(jbuf), "{\"path\":\"%s\",\"recursive\":true}", dpath);
    rc = fs_delete_tool(jbuf, res);
    CHECK(rc == 0 && res->success == 1);
    CHECK(access(dpath, F_OK) != 0);
    tool_result_free(res);

    /* 5. 根路径拒绝 */
    res = (tool_result_t *)calloc(1, sizeof(tool_result_t));
    CHECK(res != NULL);
    rc = fs_delete_tool("{\"path\":\"/\"}", res);
    CHECK(!(rc == 0 && res->success == 1));
    tool_result_free(res);

    printf("    PASSED\n");
}
#endif /* !_WIN32 */

int main(void)
{
    printf("=========================================\n");
    printf("  Tool Service Unit Tests\n");
    printf("=========================================\n");

    test_service_create_destroy();
    test_service_register_tool();
    test_service_list_tools();
    test_service_get_tool();
    test_service_unregister_tool();
#ifndef _WIN32
    test_fs_delete_builtin();
#endif

    printf("\nAll tool service tests PASSED\n");
    return 0;
}
