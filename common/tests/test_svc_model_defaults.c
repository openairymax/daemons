// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file test_svc_model_defaults.c
 * @brief svc_model_defaults_from_yaml 单元测试
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * 覆盖：
 * 1. 正常 global 段（含嵌套子段 default_retry）→ default_model/default_provider
 * 2. 无 global 段 → 输出缓冲保持空串
 * 3. 文件不存在 → AIRY_ERR_IO
 * 4. 仓库 SSoT model.yaml（真实文件，存在时）→ 与 global 段一致
 */

#include "svc_model_defaults.h"
#include "error.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PASS(name) printf("✓ %s\n", name)
#define TEST_FAIL(name, reason) printf("✗ %s: %s\n", name, reason)

static const char *TMP_YAML = "svc_model_defaults_test.yaml";

static int write_tmp_yaml(const char *content)
{
    FILE *f = fopen(TMP_YAML, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(content, 1, strlen(content), f);
    fclose(f);
    return n == strlen(content) ? 0 : -1;
}

/* global 段内 default_retry 为嵌套子段，验证嵌套结束后字段仍可解析 */
static int test_global_with_nested(void)
{
    const char *yaml =
        "providers:\n"
        "  - name: \"deepseek\"\n"
        "    models: [\"deepseek-v4-flash\"]\n"
        "global:\n"
        "  default_provider: \"deepseek\"\n"
        "  default_retry:\n"
        "    max_attempts: 3\n"
        "  default_model: \"deepseek-v4-flash\"\n"
        "  default_timeout_sec: 60\n";
    if (write_tmp_yaml(yaml) != 0) {
        TEST_FAIL("global_with_nested", "cannot write temp yaml");
        return -1;
    }
    char model[128] = {0};
    char provider[64] = {0};
    int rc = svc_model_defaults_from_yaml(TMP_YAML, model, sizeof(model),
                                          provider, sizeof(provider));
    if (rc != 0) {
        TEST_FAIL("global_with_nested", "unexpected error code");
        return -1;
    }
    if (strcmp(model, "deepseek-v4-flash") != 0) {
        char buf[192];
        snprintf(buf, sizeof(buf), "default_model=%s", model);
        TEST_FAIL("global_with_nested", buf);
        return -1;
    }
    if (strcmp(provider, "deepseek") != 0) {
        char buf[192];
        snprintf(buf, sizeof(buf), "default_provider=%s", provider);
        TEST_FAIL("global_with_nested", buf);
        return -1;
    }
    TEST_PASS("global_with_nested");
    return 0;
}

static int test_no_global_section(void)
{
    const char *yaml =
        "providers:\n"
        "  - name: \"openai\"\n"
        "    models: [\"gpt-4\"]\n";
    if (write_tmp_yaml(yaml) != 0) {
        TEST_FAIL("no_global_section", "cannot write temp yaml");
        return -1;
    }
    char model[128] = {0};
    char provider[64] = {0};
    int rc = svc_model_defaults_from_yaml(TMP_YAML, model, sizeof(model),
                                          provider, sizeof(provider));
    if (rc != 0) {
        TEST_FAIL("no_global_section", "unexpected error code");
        return -1;
    }
    if (model[0] != '\0' || provider[0] != '\0') {
        TEST_FAIL("no_global_section", "output should stay empty");
        return -1;
    }
    TEST_PASS("no_global_section");
    return 0;
}

static int test_missing_file(void)
{
    char model[128] = {0};
    char provider[64] = {0};
    int rc = svc_model_defaults_from_yaml("no_such_file_airy.yaml", model, sizeof(model),
                                          provider, sizeof(provider));
    if (rc != AIRY_ERR_IO) {
        char buf[128];
        snprintf(buf, sizeof(buf), "rc=%d, expected AIRY_ERR_IO", rc);
        TEST_FAIL("missing_file", buf);
        return -1;
    }
    TEST_PASS("missing_file");
    return 0;
}

static int test_null_args(void)
{
    int rc = svc_model_defaults_from_yaml(NULL, NULL, 0, NULL, 0);
    if (rc != AIRY_ERR_INVALID_PARAM) {
        TEST_FAIL("null_args", "NULL path should be INVALID_PARAM");
        return -1;
    }
    TEST_PASS("null_args");
    return 0;
}

/* 仓库 SSoT（真实文件）：与 global 段一致（工作目录为 daemons/common/tests） */
static int test_repo_ssot(void)
{
    const char *path = "../../../../ecosystem/manager/model/model.yaml";
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("- repo_ssot: skipped (source tree not present)\n");
        return 0;
    }
    fclose(f);
    char model[128] = {0};
    char provider[64] = {0};
    int rc = svc_model_defaults_from_yaml(path, model, sizeof(model), provider, sizeof(provider));
    if (rc != 0) {
        TEST_FAIL("repo_ssot", "unexpected error code");
        return -1;
    }
    if (model[0] == '\0') {
        TEST_FAIL("repo_ssot", "default_model empty");
        return -1;
    }
    printf("  repo_ssot: default_model=%s default_provider=%s\n", model, provider);
    TEST_PASS("repo_ssot");
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += (test_global_with_nested() != 0);
    failures += (test_no_global_section() != 0);
    failures += (test_missing_file() != 0);
    failures += (test_null_args() != 0);
    failures += (test_repo_ssot() != 0);

    if (failures == 0) {
        printf("\nAll svc_model_defaults tests passed\n");
        return 0;
    }
    printf("\n%d test(s) failed\n", failures);
    return 1;
}
