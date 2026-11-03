// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service.c
 * @brief Tool 服务核心功能单元测试
 */

#include "tool_service.h"
#include "builtin/builtin_fs_internal.h"

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

    int ret = tool_service_register(svc, &meta);
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

    tool_metadata_t *found = tool_service_get(svc, "get_test_tool");
    CHECK(found != NULL);
    CHECK(strcmp(found->name, "get_test_tool") == 0);
    tool_metadata_free(found);

    tool_service_destroy(svc);

    printf("    PASSED\n");
}

/* 社区反馈④：请求级拒绝必须把诊断文本回传模型（result 信封），
 * 模型下一轮即可改参重试或换工具。两条用例分别锁死：
 *  - 未知工具 → "did you mean" 近似建议；
 *  - 参数校验失败 → "declared params" 参数契约。 */
static void test_reject_unknown_tool_suggests(void)
{
    printf("  test_reject_unknown_tool_suggests...\n");

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "echo_tool";
    meta.name = "echo_tool";
    meta.executable = "/bin/echo";
    meta.timeout_sec = 10;
    CHECK(tool_service_register(svc, &meta) == 0);

    tool_execute_request_t req = {0};
    req.tool_id = "echo_toll"; /* 幻觉名：与 echo_tool 近似 */
    req.params_json = "{}";

    tool_result_t *res = NULL;
    int rc = tool_service_execute(svc, &req, &res);
    CHECK(rc == AIRY_ERR_EXEC_NOT_FOUND);
    CHECK(res != NULL);
    CHECK(res->success == 0);
    CHECK(res->error != NULL);
    CHECK(strstr(res->error, "did you mean") != NULL);
    CHECK(strstr(res->error, "echo_tool") != NULL);
    tool_result_free(res);

    tool_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_reject_bad_params_contract(void)
{
    printf("  test_reject_bad_params_contract...\n");

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);

    tool_param_t params[1] = {
        {"path", "{\"type\":\"string\"}", 1},
    };
    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "need_path_tool";
    meta.name = "need_path_tool";
    meta.executable = "/bin/echo";
    meta.params = params;
    meta.param_count = 1;
    meta.timeout_sec = 10;
    CHECK(tool_service_register(svc, &meta) == 0);

    tool_execute_request_t req = {0};
    req.tool_id = "need_path_tool";
    req.params_json = "{}"; /* 缺 required 参数 path */

    tool_result_t *res = NULL;
    int rc = tool_service_execute(svc, &req, &res);
    CHECK(rc == AIRY_ERR_EXEC_VALIDATION);
    CHECK(res != NULL);
    CHECK(res->success == 0);
    CHECK(res->error != NULL);
    CHECK(strstr(res->error, "declared params") != NULL);
    CHECK(strstr(res->error, "path(string, required)") != NULL);
    tool_result_free(res);

    tool_service_destroy(svc);

    printf("    PASSED\n");
}

#ifndef _WIN32
static void test_fs_delete_builtin(void)
{
    printf("  test_fs_delete_builtin...\n");

    /* CI-2：PID 隔离，避免 ctest -j 并行互踩 */
    char fpath[256];
    char dpath[256];
    char subpath[320];
    char jbuf[512];
    snprintf(fpath, sizeof(fpath), "/tmp/airy_test_fs_delete_file_%d.txt", (int)getpid());
    snprintf(dpath, sizeof(dpath), "/tmp/airy_test_fs_delete_dir_%d", (int)getpid());

    /* 先清理上次运行可能残留的状态，避免 mkdir EEXIST / 非空目录误判。 */
    unlink(fpath);
    snprintf(jbuf, sizeof(jbuf), "{\"path\":\"%s\",\"recursive\":true}", dpath);
    tool_result_t cleanup = {0};
    (void)fs_delete_tool(jbuf, 0, &cleanup);
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
    int rc = fs_delete_tool(jbuf, 0, res);
    CHECK(rc == 0 && res->success == 1);
    CHECK(access(fpath, F_OK) != 0);
    tool_result_free(res);

    /* 2. 删除空目录 */
    res = (tool_result_t *)calloc(1, sizeof(tool_result_t));
    CHECK(res != NULL);
    snprintf(jbuf, sizeof(jbuf), "{\"path\":\"%s\"}", dpath);
    rc = fs_delete_tool(jbuf, 0, res);
    CHECK(rc == 0 && res->success == 1);
    CHECK(access(dpath, F_OK) != 0);
    tool_result_free(res);

    /* 3. 非空目录非递归 → 拒绝且目录保留 */
    CHECK(mkdir(dpath, 0755) == 0);
    snprintf(subpath, sizeof(subpath), "%s/sub.txt", dpath);
    FILE *sub = fopen(subpath, "wb");
    CHECK(sub != NULL);
    fputs("x", sub);
    fclose(sub);
    res = (tool_result_t *)calloc(1, sizeof(tool_result_t));
    CHECK(res != NULL);
    snprintf(jbuf, sizeof(jbuf), "{\"path\":\"%s\"}", dpath);
    rc = fs_delete_tool(jbuf, 0, res);
    CHECK(rc == 0 && res->success == 0);
    CHECK(access(dpath, F_OK) == 0);
    tool_result_free(res);

    /* 4. 非空目录 recursive=1 → 递归删除 */
    res = (tool_result_t *)calloc(1, sizeof(tool_result_t));
    CHECK(res != NULL);
    snprintf(jbuf, sizeof(jbuf), "{\"path\":\"%s\",\"recursive\":true}", dpath);
    rc = fs_delete_tool(jbuf, 0, res);
    CHECK(rc == 0 && res->success == 1);
    CHECK(access(dpath, F_OK) != 0);
    tool_result_free(res);

    /* 5. 根路径拒绝 */
    res = (tool_result_t *)calloc(1, sizeof(tool_result_t));
    CHECK(res != NULL);
    rc = fs_delete_tool("{\"path\":\"/\"}", 0, res);
    CHECK(!(rc == 0 && res->success == 1));
    tool_result_free(res);

    printf("    PASSED\n");
}
#endif /* !_WIN32 */

#ifndef _WIN32
/* 会话内自制工具端到端：创建 → 目录可见 → 按名执行 → 重定义；并锁死三类
 * 拒绝路径（内置名抢占 / 非法名字 / 缺 shebang）。 */
static void test_tool_create_builtin(void)
{
    printf("  test_tool_create_builtin...\n");

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);

    const char *create_json =
        "{\"name\":\"hello_tool\",\"description\":\"echo greeting\","
        "\"script\":\"#!/bin/sh\\nprintf 'hi:%s' \\\"$1\\\"\\n\","
        "\"params\":[{\"name\":\"who\",\"type\":\"string\"}]}";
    tool_execute_request_t req = {0};
    req.tool_id = "tool_create";
    req.params_json = create_json;

    tool_result_t *res = NULL;
    int rc = tool_service_execute(svc, &req, &res);
    CHECK(rc == 0 && res != NULL && res->success == 1);
    CHECK(res->output != NULL);
    CHECK(strstr(res->output, "\"updated\":false") != NULL);
    tool_result_free(res);

    tool_metadata_t *m = tool_service_get(svc, "hello_tool");
    CHECK(m != NULL);
    CHECK(m->executable != NULL && strstr(m->executable, "/scripts/hello_tool") != NULL);
    CHECK(m->param_count == 1);
    tool_metadata_free(m);

    /* 落盘脚本可按名执行（JSON 参数作为 $1 传入） */
    req.tool_id = "hello_tool";
    req.params_json = "{\"who\":\"bob\"}";
    res = NULL;
    rc = tool_service_execute(svc, &req, &res);
    CHECK(rc == 0 && res != NULL && res->success == 1);
    CHECK(res->output != NULL && strstr(res->output, "hi:") != NULL);
    tool_result_free(res);

    /* 同名重定义 → updated */
    req.tool_id = "tool_create";
    req.params_json = create_json;
    res = NULL;
    rc = tool_service_execute(svc, &req, &res);
    CHECK(rc == 0 && res != NULL && res->success == 1);
    CHECK(strstr(res->output, "\"updated\":true") != NULL);
    tool_result_free(res);

    /* 拒绝：内置工具名抢占 */
    req.params_json = "{\"name\":\"fs_read\",\"description\":\"x\","
                      "\"script\":\"#!/bin/sh\\nexit 0\\n\"}";
    res = NULL;
    rc = tool_service_execute(svc, &req, &res);
    CHECK(res != NULL && res->success == 0);
    CHECK(res->error != NULL && strstr(res->error, "already registered") != NULL);
    tool_result_free(res);

    /* 拒绝：非法名字与缺 shebang */
    req.params_json = "{\"name\":\"../escape\",\"description\":\"x\","
                      "\"script\":\"#!/bin/sh\\nexit 0\\n\"}";
    res = NULL;
    rc = tool_service_execute(svc, &req, &res);
    CHECK(res != NULL && res->success == 0);
    tool_result_free(res);

    req.params_json = "{\"name\":\"no_shebang\",\"description\":\"x\","
                      "\"script\":\"exit 0\\n\"}";
    res = NULL;
    rc = tool_service_execute(svc, &req, &res);
    CHECK(res != NULL && res->success == 0);
    CHECK(res->error != NULL && strstr(res->error, "shebang") != NULL);
    tool_result_free(res);

    tool_service_destroy(svc);
    printf("    PASSED\n");
}
#endif /* !_WIN32 */

int main(void)
{
    printf("=========================================\n");
    printf("  Tool Service Unit Tests\n");
    printf("=========================================\n");

    /* T16: fs 工具在 workspace 围堵下运行；本测试的删除目标都在 /tmp。 */
    setenv("AIRY_TOOL_SANDBOX_WORKSPACE", "/tmp", 1);
    /* 自制工具落盘目录跟随 AIRY_HOME，测试固定到 /tmp 下，避免污染真实
     * 用户数据目录（路径解析首次调用即固化，故必须在所有用例之前设置）。 */
    setenv("AIRY_HOME", "/tmp/airy_tool_test_home", 1);

    test_service_create_destroy();
    test_service_register_tool();
    test_service_list_tools();
    test_service_get_tool();
    test_reject_unknown_tool_suggests();
    test_reject_bad_params_contract();
#ifndef _WIN32
    test_fs_delete_builtin();
    test_tool_create_builtin();
#endif

    printf("\nAll tool service tests PASSED\n");
    return 0;
}
