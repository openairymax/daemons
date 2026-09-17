// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_svc_model_defaults.c
 * @brief svc_model_defaults_* 单元测试
 *
 * 覆盖：
 * 1. 正常 global 段（含嵌套子段 default_retry）→ default_model/default_provider
 * 2. 无 global 段 → 输出缓冲保持空串
 * 3. 文件不存在 → AIRY_ERR_IO
 * 4. 仓库 SSoT model.yaml（真实文件，存在时）→ 与 global 段一致
 * 5. svc_tokens_parse 字面量解析
 * 6. llm / models[0] 段 max_output
 * 7. svc_model_defaults_resolve 优先级（base → 用户覆盖 → 环境变量）
 *
 * 所有临时文件写在 /tmp 隔离根下（AIRY_HOME 指向该根），不污染源码区。
 */

#include "svc_model_defaults.h"
#include "error.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_PASS(name) printf("✓ %s\n", name)
#define TEST_FAIL(name, reason) printf("✗ %s: %s\n", name, reason)

static char g_root[256];
static char g_cfg_dir[512];
static char g_tmp_yaml[512];
static char g_user_yaml[512];

static int mkdir_p(const char *path)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return (mkdir(buf, 0700) == 0 || errno == EEXIST) ? 0 : -1;
}

#define TMP_YAML (g_tmp_yaml)

static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(content, 1, strlen(content), f);
    fclose(f);
    return n == strlen(content) ? 0 : -1;
}

static int write_tmp_yaml(const char *content)
{
    if (write_file(TMP_YAML, content) != 0) {
        return -1;
    }
    return 0;
}

static int test_global_with_nested(void)
{
    const char *yaml = "providers:\n"
                       "  - name: \"deepseek\"\n"
                       "    models: [\"deepseek-flash\"]\n"
                       "global:\n"
                       "  default_provider: \"deepseek\"\n"
                       "  default_retry:\n"
                       "    max_attempts: 3\n"
                       "  default_model: \"deepseek-flash\"\n"
                       "  default_timeout_sec: 60\n";
    if (write_tmp_yaml(yaml) != 0) {
        TEST_FAIL("global_with_nested", "cannot write temp yaml");
        return -1;
    }
    char model[128] = {0};
    char provider[64] = {0};
    int rc =
        svc_model_defaults_from_yaml(TMP_YAML, model, sizeof(model), provider, sizeof(provider));
    if (rc != 0) {
        TEST_FAIL("global_with_nested", "unexpected error code");
        return -1;
    }
    if (strcmp(model, "deepseek-flash") != 0) {
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
    const char *yaml = "providers:\n"
                       "  - name: \"openai\"\n"
                       "    models: [\"gpt-4\"]\n";
    if (write_tmp_yaml(yaml) != 0) {
        TEST_FAIL("no_global_section", "cannot write temp yaml");
        return -1;
    }
    char model[128] = {0};
    char provider[64] = {0};
    int rc =
        svc_model_defaults_from_yaml(TMP_YAML, model, sizeof(model), provider, sizeof(provider));
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
    int rc = svc_model_defaults_from_yaml("no_such_file_airy.yaml", model, sizeof(model), provider,
                                          sizeof(provider));
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

static int test_tokens_parse(void)
{
    struct {
        const char *text;
        int want;
    } cases[] = {
        {"2048", 2048},   {"4k", 4096},      {"16k", 16384},   {"128k", 131072},
        {"1M", 1048576},  {"2m", 2097152},   {" 8k ", 8192},   {"16K", 16384},
        {"", 0},          {"abc", 0},        {"0", 0},         {"-4k", 0},
        {"16x", 0},       {"16k extra", 0},  {NULL, 0},
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; ++i) {
        int got = svc_tokens_parse(cases[i].text);
        if (got != cases[i].want) {
            char buf[160];
            snprintf(buf, sizeof(buf), "svc_tokens_parse(\"%s\")=%d, want %d",
                     cases[i].text ? cases[i].text : "(null)", got, cases[i].want);
            TEST_FAIL("tokens_parse", buf);
            return -1;
        }
    }
    TEST_PASS("tokens_parse");
    return 0;
}

static int test_llm_section_max_output(void)
{
    const char *yaml = "llm:\n"
                       "  api_format: \"openai\"\n"
                       "  base_url: \"https://api.example.com/v1\"\n"
                       "  api_key_env: \"EXAMPLE_API_KEY\"\n"
                       "  model: \"deepseek-flash\"\n"
                       "  max_output: \"16k\"\n";
    if (write_tmp_yaml(yaml) != 0) {
        TEST_FAIL("llm_section_max_output", "cannot write temp yaml");
        return -1;
    }
    svc_model_llm_config_t out;
    memset(&out, 0, sizeof(out));
    int rc = svc_model_defaults_llm_from_yaml(TMP_YAML, &out);
    if (rc != 0) {
        TEST_FAIL("llm_section_max_output", "unexpected error code");
        return -1;
    }
    if (strcmp(out.model, "deepseek-flash") != 0 || out.max_output_tokens != 16384) {
        char buf[192];
        snprintf(buf, sizeof(buf), "model=%s max_output_tokens=%d", out.model,
                 out.max_output_tokens);
        TEST_FAIL("llm_section_max_output", buf);
        return -1;
    }
    TEST_PASS("llm_section_max_output");
    return 0;
}

static int test_models0_max_output(void)
{
    const char *yaml = "models:\n"
                       "  - model_id: \"deepseek-flash\"\n"
                       "    api_format: \"openai\"\n"
                       "    base_url: \"https://api.example.com/v1\"\n"
                       "    max_output: \"256k\"\n";
    if (write_tmp_yaml(yaml) != 0) {
        TEST_FAIL("models0_max_output", "cannot write temp yaml");
        return -1;
    }
    svc_model_llm_config_t out;
    memset(&out, 0, sizeof(out));
    int rc = svc_model_defaults_models0_from_yaml(TMP_YAML, &out);
    if (rc != 0) {
        TEST_FAIL("models0_max_output", "unexpected error code");
        return -1;
    }
    if (strcmp(out.model, "deepseek-flash") != 0 || out.max_output_tokens != 262144) {
        char buf[192];
        snprintf(buf, sizeof(buf), "model=%s max_output_tokens=%d", out.model,
                 out.max_output_tokens);
        TEST_FAIL("models0_max_output", buf);
        return -1;
    }
    TEST_PASS("models0_max_output");
    return 0;
}

/* 用户覆盖文件：$AIRY_HOME/config/model.yaml */
static int user_model_set(const char *content)
{
    return write_file(g_user_yaml, content);
}

static void user_model_clear(void)
{
    remove(g_user_yaml);
}

static int resolve_check(const char *tag, const char *base_model, const char *base_provider,
                         const char *want_model, const char *want_provider)
{
    char model[128] = {0};
    char provider[64] = {0};
    int rc = svc_model_defaults_resolve(base_model, base_provider, model, sizeof(model), provider,
                                        sizeof(provider));
    if (rc != 0) {
        char buf[160];
        snprintf(buf, sizeof(buf), "rc=%d", rc);
        TEST_FAIL(tag, buf);
        return -1;
    }
    if (strcmp(model, want_model) != 0 || strcmp(provider, want_provider) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "got model=%s provider=%s, want model=%s provider=%s", model,
                 provider, want_model, want_provider);
        TEST_FAIL(tag, buf);
        return -1;
    }
    TEST_PASS(tag);
    return 0;
}

static int test_resolve_precedence(void)
{
    /* 1. 无用户覆盖、无环境变量：调用方 base 生效，供应商沿用调用方 */
    user_model_clear();
    unsetenv("AIRY_AGENT_MODEL");
    if (resolve_check("resolve_base_wins", "base-model", "base-prov", "base-model", "base-prov") != 0)
        return -1;

    /* 2. 调用方未提供模型：内建兜底，供应商保持空 */
    if (resolve_check("resolve_builtin_fallback", NULL, NULL, SVC_MODEL_DEFAULT_FALLBACK, "") != 0)
        return -1;

    /* 3. 用户覆盖 global 段：覆盖调用方 base（用户优先） */
    if (user_model_set("global:\n"
                       "  default_provider: \"user-prov\"\n"
                       "  default_model: \"user-model\"\n") != 0) {
        TEST_FAIL("resolve_user_global", "cannot write user model.yaml");
        return -1;
    }
    if (resolve_check("resolve_user_global", "base-model", "base-prov", "user-model", "user-prov") !=
        0)
        return -1;

    /* 4. 用户覆盖 llm 段：global 缺省时回退 llm.model */
    if (user_model_set("llm:\n"
                       "  model: \"llm-model\"\n") != 0) {
        TEST_FAIL("resolve_user_llm", "cannot write user model.yaml");
        return -1;
    }
    if (resolve_check("resolve_user_llm", NULL, NULL, "llm-model", "") != 0)
        return -1;

    /* 5. 用户覆盖 models[0] 段：global / llm 均缺省时回退 models[0].model_id */
    if (user_model_set("models:\n"
                       "  - model_id: \"m0-model\"\n"
                       "    api_format: \"openai\"\n") != 0) {
        TEST_FAIL("resolve_user_models0", "cannot write user model.yaml");
        return -1;
    }
    if (resolve_check("resolve_user_models0", NULL, NULL, "m0-model", "") != 0)
        return -1;

    /* 6. 环境变量最高优先（用户覆盖的模型被环境变量顶掉，供应商仍取用户覆盖） */
    if (user_model_set("global:\n"
                       "  default_provider: \"user-prov\"\n"
                       "  default_model: \"user-model\"\n") != 0) {
        TEST_FAIL("resolve_env_wins", "cannot write user model.yaml");
        return -1;
    }
    setenv("AIRY_AGENT_MODEL", "env-model", 1);
    int rc = resolve_check("resolve_env_wins", "base-model", "base-prov", "env-model", "user-prov");
    unsetenv("AIRY_AGENT_MODEL");
    if (rc != 0)
        return -1;

    user_model_clear();
    return 0;
}

static int test_resolve_invalid(void)
{
    char provider[64] = {0};
    int rc = svc_model_defaults_resolve(NULL, NULL, NULL, 0, provider, sizeof(provider));
    if (rc != AIRY_ERR_INVALID_PARAM) {
        TEST_FAIL("resolve_invalid_param", "NULL out_model should be INVALID_PARAM");
        return -1;
    }
    TEST_PASS("resolve_invalid_param");
    return 0;
}

int main(void)
{
    int failures = 0;

    snprintf(g_root, sizeof(g_root), "/tmp/airy_svc_model_defaults_%ld", (long)getpid());
    if (mkdir_p(g_root) != 0) {
        printf("cannot create test root %s\n", g_root);
        return 1;
    }
    snprintf(g_cfg_dir, sizeof(g_cfg_dir), "%s/config", g_root);
    if (mkdir_p(g_cfg_dir) != 0) {
        printf("cannot create test config dir %s\n", g_cfg_dir);
        return 1;
    }
    snprintf(g_tmp_yaml, sizeof(g_tmp_yaml), "%s/svc_model_defaults_test.yaml", g_root);
    snprintf(g_user_yaml, sizeof(g_user_yaml), "%s/model.yaml", g_cfg_dir);
    /* 让 airy_config_dir() 落在隔离根内，避免读写用户真实配置 */
    setenv("AIRY_HOME", g_root, 1);

    failures += (test_global_with_nested() != 0);
    failures += (test_no_global_section() != 0);
    failures += (test_missing_file() != 0);
    failures += (test_null_args() != 0);
    failures += (test_repo_ssot() != 0);
    failures += (test_tokens_parse() != 0);
    failures += (test_llm_section_max_output() != 0);
    failures += (test_models0_max_output() != 0);
    failures += (test_resolve_precedence() != 0);
    failures += (test_resolve_invalid() != 0);

    if (failures == 0) {
        printf("\nAll svc_model_defaults tests passed\n");
        return 0;
    }
    printf("\n%d test(s) failed\n", failures);
    return 1;
}
