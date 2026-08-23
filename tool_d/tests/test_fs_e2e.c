// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_fs_e2e.c
 * @brief 认知层→执行层端到端文件增删改查（2.4.4，coding 场景）。
 *
 * 模拟 Agent coding 会话中的决策序列，通过 tool_service_execute 走完整
 * 执行链路（ACL 授权 → Cupolas 安全审批 → sandbox → executor → builtin），
 * 验证 fs_write / fs_read / fs_edit / fs_glob / fs_grep / fs_list /
 * fs_delete 在真实文件系统上的增删改查闭环，并覆盖 fail-closed 安全语义。
 *
 * 覆盖：
 *   1. 创建：fs_write 写入 C 源码（coding 场景）
 *   2. 读取：fs_read 内容与写入一致
 *   3. 编辑：fs_edit 精确替换修复 bug（return 0 -> EXIT_SUCCESS）
 *   4. 复核：fs_read 确认修改生效
 *   5. 定位：fs_glob 按模式定位项目文件
 *   6. 搜索：fs_grep 内容检索命中关键字
 *   7. 列表：fs_list 目录清单包含目标文件
 *   8. 删除：fs_delete 清理临时文件
 *   9. 确认：fs_read 返回 NOT_FOUND（删除真实生效）
 *  10. 安全：未授权 agent 调用被 fail-closed 拒绝
 *  11. 未知工具：返回 TOOL_NOT_FOUND
 *  12. 参数缺失：返回 TOOL_VALIDATION
 *
 * @note 不使用 assert() 执行副作用操作：Release 构建定义 NDEBUG，会把
 *       assert(expr) 展开为 ((void)0)，导致副作用表达式不执行。所有
 *       检查使用 CHECK（abort 版，任意构建类型下都执行）。详见
 *       project_memory.md 的 assert/NDEBUG heisenbug 教训。
 */

#include "tool_service.h"
#include "daemon_security.h"
#include "error.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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

#define E2E_DIR  "/tmp/airy_e2e_coding"
#define E2E_FILE E2E_DIR "/main.c"

/* coding 场景：Agent 创建的一个 C 源文件 */
static const char kSource[] =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "\n"
    "int main(void)\n"
    "{\n"
    "    printf(\"hello airymax\\n\");\n"
    "    return 0;\n"
    "}\n";

static const char *kEditOld = "return 0;";
static const char *kEditNew = "return EXIT_SUCCESS;";

static int g_checks = 0;
static int g_fails = 0;

#define TEST(cond, name)                          \
    do {                                          \
        g_checks++;                               \
        if (cond) {                               \
            printf("  [PASS] %s\n", name);        \
        } else {                                  \
            g_fails++;                            \
            printf("  [FAIL] %s (%s:%d)\n", name, \
                   __FILE__, __LINE__);           \
        }                                         \
    } while (0)

/* 走 tool_service_execute 完整链路执行工具；params 为 cJSON 对象，
 * 自动序列化（字符串转义由 cJSON 处理）。返回值与 res 均由调用者处理。 */
static tool_result_t *run_tool(tool_service_t *svc, const char *tool_id,
                               const char *agent_id, cJSON *params, int *ret)
{
    char *json = cJSON_PrintUnformatted(params);
    CHECK(json != NULL);

    tool_execute_request_t req;
    memset(&req, 0, sizeof(req));
    req.tool_id = tool_id;
    req.params_json = json;
    req.agent_id = agent_id;

    tool_result_t *res = NULL;
    *ret = tool_service_execute(svc, &req, &res);
    free(json);
    return res;
}

static void test_e2e_coding_crud(tool_service_t *svc)
{
    printf("\n[阶段 1] coding 场景文件增删改查（完整链路）\n");

    /* 0. 清理上次运行残留并重建测试目录 */
#ifndef _WIN32
    unlink(E2E_FILE);
    rmdir(E2E_DIR);
    CHECK(mkdir(E2E_DIR, 0755) == 0);
#endif

    /* 1. 创建：认知层决策"写入源码文件" → fs_write */
    cJSON *p = cJSON_CreateObject();
    CHECK(p != NULL);
    cJSON_AddStringToObject(p, "path", E2E_FILE);
    cJSON_AddStringToObject(p, "content", kSource);
    int ret = -999;
    tool_result_t *res = run_tool(svc, "fs_write", "tool_d", p, &ret);
    cJSON_Delete(p);
    TEST(ret == 0 && res && res->success == 1, "fs_write 创建 main.c");
    if (res) {
        printf("    fs_write -> %s\n", res->output ? res->output : "(no output)");
        tool_result_free(res);
    }

    /* 2. 读取：认知层决策"读回验证" → fs_read */
    p = cJSON_CreateObject();
    CHECK(p != NULL);
    cJSON_AddStringToObject(p, "path", E2E_FILE);
    res = run_tool(svc, "fs_read", "tool_d", p, &ret);
    cJSON_Delete(p);
    TEST(ret == 0 && res && res->success == 1 &&
             res->output && strcmp(res->output, kSource) == 0,
         "fs_read 内容与写入一致");
    if (res) {
        tool_result_free(res);
    }

    /* 3. 编辑：认知层决策"修复 return 0 的遗留写法" → fs_edit */
    p = cJSON_CreateObject();
    CHECK(p != NULL);
    cJSON_AddStringToObject(p, "path", E2E_FILE);
    cJSON_AddStringToObject(p, "old", kEditOld);
    cJSON_AddStringToObject(p, "new", kEditNew);
    res = run_tool(svc, "fs_edit", "tool_d", p, &ret);
    cJSON_Delete(p);
    TEST(ret == 0 && res && res->success == 1, "fs_edit 精确替换生效");
    if (res) {
        printf("    fs_edit -> %s\n", res->output ? res->output : "(no output)");
        tool_result_free(res);
    }

    /* 4. 复核：fs_read 确认修改已落地 */
    p = cJSON_CreateObject();
    CHECK(p != NULL);
    cJSON_AddStringToObject(p, "path", E2E_FILE);
    res = run_tool(svc, "fs_read", "tool_d", p, &ret);
    cJSON_Delete(p);
    TEST(ret == 0 && res && res->success == 1 && res->output &&
             strstr(res->output, kEditNew) != NULL && strstr(res->output, kEditOld) == NULL,
         "fs_read 复核：EXIT_SUCCESS 生效且 return 0 已替换");
    if (res) {
        tool_result_free(res);
    }

    /* 5. 定位：fs_glob 按模式查找项目文件 */
    p = cJSON_CreateObject();
    CHECK(p != NULL);
    cJSON_AddStringToObject(p, "pattern", "*.c");
    cJSON_AddStringToObject(p, "base", E2E_DIR);
    res = run_tool(svc, "fs_glob", "tool_d", p, &ret);
    cJSON_Delete(p);
    TEST(ret == 0 && res && res->success == 1 && res->output &&
             strstr(res->output, "main.c") != NULL,
         "fs_glob 定位到 main.c");
    if (res) {
        printf("    fs_glob -> %s\n", res->output ? res->output : "(no output)");
        tool_result_free(res);
    }

    /* 6. 搜索：fs_grep 内容检索命中关键字 */
    p = cJSON_CreateObject();
    CHECK(p != NULL);
    cJSON_AddStringToObject(p, "pattern", "hello airymax");
    cJSON_AddStringToObject(p, "path", E2E_DIR);
    res = run_tool(svc, "fs_grep", "tool_d", p, &ret);
    cJSON_Delete(p);
    TEST(ret == 0 && res && res->success == 1 && res->output &&
             strstr(res->output, "main.c") != NULL &&
             strstr(res->output, "hello airymax") != NULL,
         "fs_grep 命中 main.c 中的关键字");
    if (res) {
        printf("    fs_grep -> %s\n", res->output ? res->output : "(no output)");
        tool_result_free(res);
    }

    /* 7. 列表：fs_list 目录清单包含目标文件 */
    p = cJSON_CreateObject();
    CHECK(p != NULL);
    cJSON_AddStringToObject(p, "path", E2E_DIR);
    res = run_tool(svc, "fs_list", "tool_d", p, &ret);
    cJSON_Delete(p);
    TEST(ret == 0 && res && res->success == 1 && res->output &&
             strstr(res->output, "main.c") != NULL,
         "fs_list 目录清单包含 main.c");
    if (res) {
        printf("    fs_list -> %s\n", res->output ? res->output : "(no output)");
        tool_result_free(res);
    }

    /* 8. 删除：fs_delete 清理临时文件 */
    p = cJSON_CreateObject();
    CHECK(p != NULL);
    cJSON_AddStringToObject(p, "path", E2E_FILE);
    res = run_tool(svc, "fs_delete", "tool_d", p, &ret);
    cJSON_Delete(p);
#ifndef _WIN32
    TEST(ret == 0 && res && res->success == 1 && access(E2E_FILE, F_OK) != 0,
         "fs_delete 文件真实删除");
#else
    TEST(ret == 0 && res && res->success == 1, "fs_delete 文件真实删除");
#endif
    if (res) {
        printf("    fs_delete -> %s\n", res->output ? res->output : "(no output)");
        tool_result_free(res);
    }

    /* 9. 确认：fs_read 返回 NOT_FOUND */
    p = cJSON_CreateObject();
    CHECK(p != NULL);
    cJSON_AddStringToObject(p, "path", E2E_FILE);
    res = run_tool(svc, "fs_read", "tool_d", p, &ret);
    cJSON_Delete(p);
    TEST(ret == AIRY_ERR_NOT_FOUND, "fs_read 确认文件已不存在 (NOT_FOUND)");
    if (res) {
        tool_result_free(res);
    }
}

static void test_e2e_fail_closed(tool_service_t *svc)
{
    printf("\n[阶段 2] fail-closed 安全语义\n");

    /* 10. 未授权 agent 调用 fs_read → 审批拒绝（EPERM） */
    cJSON *p = cJSON_CreateObject();
    CHECK(p != NULL);
    cJSON_AddStringToObject(p, "path", E2E_FILE);
    int ret = -999;
    tool_result_t *res = run_tool(svc, "fs_read", "e2e_unauthorized", p, &ret);
    cJSON_Delete(p);
    TEST(ret == AIRY_EPERM, "未授权 agent 被 fail-closed 拒绝 (EPERM)");
    if (res) {
        tool_result_free(res);
    }
}

static void test_e2e_not_found(tool_service_t *svc)
{
    printf("\n[阶段 3] 工具注册表语义\n");

    /* 11. 未知工具 → TOOL_NOT_FOUND */
    cJSON *p = cJSON_CreateObject();
    CHECK(p != NULL);
    int ret = -999;
    tool_result_t *res = run_tool(svc, "no_such_tool", "tool_d", p, &ret);
    cJSON_Delete(p);
    TEST(ret == AIRY_ERROR_TOOL_NOT_FOUND, "未知工具返回 TOOL_NOT_FOUND");
    if (res) {
        tool_result_free(res);
    }
}

static void test_e2e_validation(tool_service_t *svc)
{
    printf("\n[阶段 4] 参数校验语义\n");

    /* 12. fs_read 缺 path 参数 → VALIDATION */
    cJSON *p = cJSON_CreateObject();
    CHECK(p != NULL);
    int ret = -999;
    tool_result_t *res = run_tool(svc, "fs_read", "tool_d", p, &ret);
    cJSON_Delete(p);
    TEST(ret == AIRY_ERROR_TOOL_VALIDATION, "缺少必填参数返回 TOOL_VALIDATION");
    if (res) {
        tool_result_free(res);
    }
}

int main(void)
{
    printf("=== t9/2.4.4: 认知层→执行层端到端（tool_service_execute 完整链路）===\n\n");

    /* 初始化安全域（fail-closed ACL 的宿主） */
    CHECK(daemon_security_init(NULL, NULL) == 0);

    /* 授权 coding 会话所需的全部文件工具 */
    static const char *kAuthorized[] = {
        "fs_read", "fs_write", "fs_edit", "fs_glob", "fs_grep", "fs_list", "fs_delete",
    };
    for (size_t i = 0; i < sizeof(kAuthorized) / sizeof(kAuthorized[0]); i++) {
        int ar = daemon_security_add_acl_rule("tool_d", kAuthorized[i], true);
        CHECK(ar == 0);
    }

    tool_service_t *svc = tool_service_create(NULL);
    CHECK(svc != NULL);

    test_e2e_coding_crud(svc);
    test_e2e_fail_closed(svc);
    test_e2e_not_found(svc);
    test_e2e_validation(svc);

    tool_service_destroy(svc);

    printf("\n=== 结果：%d/%d 通过 ===\n", g_checks - g_fails, g_checks);
    return g_fails == 0 ? 0 : 1;
}
