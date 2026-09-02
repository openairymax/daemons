// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_plugin_discovery.c
 * @brief tool_d 插件域离线扩展校验器单元测试（0.1.6 P1-5）。
 *
 * 覆盖 plugin_discovery_validate_plugin() 的 fail-closed 校验面：
 * - 合法插件（manifest schema 齐全 + 库文件存在 + 权限非空）通过
 * - 目录不存在 / manifest 缺失 / type 非法 / 库缺失 / 无权限声明拒绝
 * - 任一不满足即 invalid，绝不部分接受
 *
 * 全部在 /tmp 隔离目录内构造，绝不触碰真实插件目录。
 */

// @owner: team-B
#include "plugin_discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("    FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, msg); \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* 递归建目录（父链可能不存在） */
static void mkdir_p(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void write_file(const char *path, const char *content)
{
    FILE *fp = fopen(path, "w");
    if (fp) {
        fputs(content, fp);
        fclose(fp);
    }
}

#define T_ROOT "/tmp/airyt_plugin_disc"

static void test_valid_plugin(void)
{
    printf("  test_valid_plugin...\n");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/valid", T_ROOT);
    mkdir_p(dir);

    /* 库文件必须存在（任意常规文件即可，校验只查存在性） */
    write_file("/tmp/airyt_plugin_disc/valid/libtest.so", "ELF fake");

    const char *manifest =
        "name: test_plugin\n"
        "version: 0.1.0\n"
        "author: SPHARX\n"
        "description: offline validator test\n"
        "type: tool_provider\n"
        "api_version: 1\n"
        "min_airy_version: 1\n"
        "library: libtest.so\n"
        "permissions:\n"
        "  - file_read\n"
        "  - tool_execute\n";
    write_file("/tmp/airyt_plugin_disc/valid/manifest.yaml", manifest);

    plugin_discovery_result_t res;
    int ret = plugin_discovery_validate_plugin(dir, &res);
    CHECK(ret == 0, "valid plugin returns 0");
    CHECK(res.valid, "valid plugin flagged valid");
    CHECK(strcmp(res.name, "test_plugin") == 0, "name parsed");
    CHECK(res.permission_count == 2, "permissions parsed");
    CHECK(strcmp(res.library_path, "/tmp/airyt_plugin_disc/valid/libtest.so") == 0,
          "library_path resolved under plugin dir");
}

static void test_missing_dir(void)
{
    printf("  test_missing_dir...\n");
    plugin_discovery_result_t res;
    int ret = plugin_discovery_validate_plugin("/tmp/airyt_plugin_disc/no_such_dir", &res);
    CHECK(ret != 0, "missing dir rejected");
    CHECK(!res.valid, "missing dir invalid");
}

static void test_missing_manifest(void)
{
    printf("  test_missing_manifest...\n");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/nomanifest", T_ROOT);
    mkdir_p(dir);
    plugin_discovery_result_t res;
    int ret = plugin_discovery_validate_plugin(dir, &res);
    CHECK(ret != 0, "missing manifest rejected");
    CHECK(!res.valid, "missing manifest invalid");
    CHECK(strstr(res.error_reason, "manifest") != NULL, "error names manifest");
}

static void test_invalid_type(void)
{
    printf("  test_invalid_type...\n");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/badtype", T_ROOT);
    mkdir_p(dir);
    write_file("/tmp/airyt_plugin_disc/badtype/libtest.so", "x");
    write_file("/tmp/airyt_plugin_disc/badtype/manifest.yaml",
               "name: bad_type\n"
               "type: not_a_plugin_type\n"
               "api_version: 1\n"
               "min_airy_version: 1\n"
               "library: libtest.so\n"
               "permissions:\n"
               "  - file_read\n");
    plugin_discovery_result_t res;
    int ret = plugin_discovery_validate_plugin(dir, &res);
    CHECK(ret != 0, "invalid type rejected");
    CHECK(!res.valid, "invalid type invalid");
}

static void test_missing_library(void)
{
    printf("  test_missing_library...\n");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/nolib", T_ROOT);
    mkdir_p(dir);
    /* manifest 引用不存在的库文件 */
    write_file("/tmp/airyt_plugin_disc/nolib/manifest.yaml",
               "name: no_lib\n"
               "type: tool_provider\n"
               "api_version: 1\n"
               "min_airy_version: 1\n"
               "library: libghost.so\n"
               "permissions:\n"
               "  - file_read\n");
    plugin_discovery_result_t res;
    int ret = plugin_discovery_validate_plugin(dir, &res);
    CHECK(ret != 0, "missing library rejected");
    CHECK(!res.valid, "missing library invalid");
    CHECK(strstr(res.error_reason, "Library not found") != NULL, "error names library");
}

static void test_no_permissions(void)
{
    printf("  test_no_permissions...\n");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/noperm", T_ROOT);
    mkdir_p(dir);
    write_file("/tmp/airyt_plugin_disc/noperm/libtest.so", "x");
    /* 缺 permissions 声明（fail-closed 拒绝，防越权后门） */
    write_file("/tmp/airyt_plugin_disc/noperm/manifest.yaml",
               "name: no_perm\n"
               "type: tool_provider\n"
               "api_version: 1\n"
               "min_airy_version: 1\n"
               "library: libtest.so\n");
    plugin_discovery_result_t res;
    int ret = plugin_discovery_validate_plugin(dir, &res);
    CHECK(ret != 0, "no permissions rejected");
    CHECK(!res.valid, "no permissions invalid");
    CHECK(strstr(res.error_reason, "permissions") != NULL, "error names permissions");
}

static void test_null_args(void)
{
    printf("  test_null_args...\n");
    plugin_discovery_result_t res;
    int ret = plugin_discovery_validate_plugin(NULL, &res);
    CHECK(ret != 0 && !res.valid, "NULL dir rejected");
}

int main(void)
{
    printf("test_plugin_discovery: offline validator (P1-5)\n");
    test_valid_plugin();
    test_missing_dir();
    test_missing_manifest();
    test_invalid_type();
    test_missing_library();
    test_no_permissions();
    test_null_args();
    printf("  %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
