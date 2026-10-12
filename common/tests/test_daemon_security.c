// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_daemon_security.c - Daemon Security Module Unit Tests
 */

#include "../include/daemon_security.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)               \
    do {                         \
        tests_run++;             \
        printf("  %-50s", name); \
    } while (0)
#define PASS()              \
    do {                    \
        tests_passed++;     \
        printf("[PASS]\n"); \
    } while (0)
#define FAIL(msg)                   \
    do {                            \
        printf("[FAIL] %s\n", msg); \
        return;                     \
    } while (0)
#define ASSERT(cond, msg) \
    do {                  \
        if (!(cond)) {    \
            FAIL(msg);    \
        }                 \
    } while (0)

static void test_init_null_config(void)
{
    TEST("Init with NULL config");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    int ret = daemon_security_init(NULL, &err);
    ASSERT(ret == 0, "init with NULL config should succeed");
    daemon_security_shutdown();
    PASS();
}

static void test_double_init(void)
{
    TEST("Double init is idempotent");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    int ret = daemon_security_init(NULL, &err);
    ASSERT(ret == 0, "first init");
    ret = daemon_security_init(NULL, &err);
    ASSERT(ret == 0, "second init should be idempotent");
    daemon_security_shutdown();
    PASS();
}

static void test_shutdown_without_init(void)
{
    TEST("Shutdown without init is safe");
    daemon_security_shutdown();
    daemon_security_shutdown();
    PASS();
}

static void test_sanitize_llm_input_normal(void)
{
    TEST("Sanitize LLM input - normal text");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    char output[256];
    const char *input = "Hello world, how are you today?";
    int ret = daemon_sanitize_llm_input(input, output, sizeof(output));
    ASSERT(ret == 0, "sanitize should succeed");
    ASSERT(strlen(output) > 0, "output should not be empty");
    ASSERT(strstr(output, "Hello") != NULL, "normal text should pass through");

    daemon_security_shutdown();
    PASS();
}

static void test_sanitize_llm_input_dangerous(void)
{
    TEST("Sanitize LLM input - dangerous pattern");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    char output[256];
    const char *input = "run command; rm -rf /";
    int ret = daemon_sanitize_llm_input(input, output, sizeof(output));
    ASSERT(ret != 0, "dangerous input should be rejected");
    ASSERT(strstr(output, "SANITIZED") != NULL || strstr(output, "rejected") != NULL,
           "output should indicate sanitization");

    daemon_security_shutdown();
    PASS();
}

static void test_sanitize_llm_input_null_params(void)
{
    TEST("Sanitize LLM input - null params rejected");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    char output[256];
    int ret = daemon_sanitize_llm_input(NULL, output, sizeof(output));
    ASSERT(ret != 0, "null input should be rejected");

    ret = daemon_sanitize_llm_input("test", NULL, sizeof(output));
    ASSERT(ret != 0, "null output should be rejected");

    ret = daemon_sanitize_llm_input("test", output, 0);
    ASSERT(ret != 0, "zero output_size should be rejected");

    daemon_security_shutdown();
    PASS();
}

static void test_sanitize_tool_params_normal(void)
{
    TEST("Sanitize tool params - normal");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    char sanitized_tool[128];
    char sanitized_params[256];
    int ret = daemon_sanitize_tool_params("read_file", "{\"path\":\"/tmp/test\"}", sanitized_tool,
                                          sizeof(sanitized_tool), sanitized_params,
                                          sizeof(sanitized_params));
    ASSERT(ret == 0, "sanitize tool params should succeed");
    ASSERT(strstr(sanitized_tool, "read_file") != NULL, "tool name should be preserved");

    daemon_security_shutdown();
    PASS();
}

static void test_sanitize_tool_params_dangerous(void)
{
    TEST("Sanitize tool params - shell-like data is not rejected (0.1.14)");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    char sanitized_tool[128];
    char sanitized_params[256];
    /* 0.1.14 语义变更：SEC-014 注入模式子串扫描已移除——工具参数是数据，
     * 含 ; | > 等字符的合法数据（fs_write 写 HTML、grep 正则）此前被整段
     * 误杀（0.1.13 实机回归）。sanitize 层仅做控制字符剥离留痕（审计/展示），
     * 拒绝职责由 ACL + tool_approval + 沙箱配额承担（白名单化记入 0.2.x）。 */
    int ret = daemon_sanitize_tool_params("exec", "; cat /etc/passwd", sanitized_tool,
                                          sizeof(sanitized_tool), sanitized_params,
                                          sizeof(sanitized_params));
    ASSERT(ret == 0, "tool params are data: shell-like content must not be rejected here");
    ASSERT(strchr(sanitized_params, ';') != NULL, "sanitized copy keeps displayable content");

    daemon_security_shutdown();
    PASS();
}

static void test_sanitize_tool_params_null(void)
{
    TEST("Sanitize tool params - null params rejected");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    char buf[64];
    int ret = daemon_sanitize_tool_params(NULL, "{}", buf, 64, buf, 64);
    ASSERT(ret != 0, "null tool_name should be rejected");

    ret = daemon_sanitize_tool_params("tool", NULL, buf, 64, buf, 64);
    ASSERT(ret != 0, "null params should be rejected");

    daemon_security_shutdown();
    PASS();
}

static void test_check_tool_permission_no_acl(void)
{
    TEST("Check tool permission - no ACL entry denies");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    int allowed = daemon_check_tool_permission("agent_001", "file_read", "execute");
    ASSERT(allowed != 0, "no ACL entry should deny by default");

    daemon_security_shutdown();
    PASS();
}

static void test_check_tool_permission_null_params(void)
{
    TEST("Check tool permission - null params denied");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    int allowed = daemon_check_tool_permission(NULL, "tool", "execute");
    ASSERT(allowed != 0, "null agent_id should be denied");

    allowed = daemon_check_tool_permission("agent", NULL, "execute");
    ASSERT(allowed != 0, "null tool_name should be denied");

    allowed = daemon_check_tool_permission("agent", "tool", NULL);
    ASSERT(allowed != 0, "null action should be denied");

    daemon_security_shutdown();
    PASS();
}

static void test_audit_log_event(void)
{
    TEST("Audit log event");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    int ret = daemon_audit_log_event("tool_d", "execute", "/tool/read_file", 0, "agent_001");
    ASSERT(ret == 0, "audit success event should succeed");

    ret = daemon_audit_log_event("llm_d", "query_failed", "/llm/gpt-4", -1, "agent_002");
    ASSERT(ret == 0, "audit failure event should succeed");

    daemon_security_shutdown();
    PASS();
}

static void test_audit_log_event_null_params(void)
{
    TEST("Audit log event - null params handled");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    int ret = daemon_audit_log_event(NULL, "op", "res", 0, "agent");
    ASSERT(ret != 0, "null service_name should be rejected");

    ret = daemon_audit_log_event("svc", NULL, "res", 0, "agent");
    ASSERT(ret != 0, "null operation should be rejected");

    daemon_security_shutdown();
    PASS();
}

static void test_sanitize_uninitialized_fail_safe(void)
{
    TEST("Sanitize works uninitialized (fail-safe)");
    daemon_security_shutdown();

    char output[256];
    int ret = daemon_sanitize_llm_input("hello world", output, sizeof(output));
    ASSERT(ret == 0, "uninitialized sanitize should still succeed (fail-safe)");

    daemon_security_shutdown();
    PASS();
}

static void test_init_with_config(void)
{
    TEST("Init with explicit config");
    daemon_security_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sanitize_level = SANITIZE_LEVEL_HIGH;
    cfg.enable_permission_cache = true;
    cfg.enable_vault = true;
    cfg.enable_audit_logging = true;

    airy_err_t err;
    memset(&err, 0, sizeof(err));
    int ret = daemon_security_init(&cfg, &err);
    ASSERT(ret == 0, "init with config should succeed");

    daemon_security_shutdown();
    PASS();
}

static void test_sanitize_llm_input_special_chars(void)
{
    TEST("Sanitize LLM input - tab chars stripped");
    airy_err_t err;
    memset(&err, 0, sizeof(err));
    daemon_security_init(NULL, &err);

    char output[256];
    const char *input = "hello\tworld\twith\ttabs";
    int ret = daemon_sanitize_llm_input(input, output, sizeof(output));
    ASSERT(ret == 0, "sanitize should succeed");
    ASSERT(strchr(output, '\t') == NULL, "tabs should be removed");

    daemon_security_shutdown();
    PASS();
}

static void test_rules_file_loading(void)
{
    TEST("Load tool permission rules file");
    /* CI-2：PID 隔离，避免 ctest -j 并行互踩 */
    char path[256];
    snprintf(path, sizeof(path), "/tmp/airy_test_permission_rules_%d.yaml", (int)getpid());
    FILE *fp = fopen(path, "w");
    ASSERT(fp != NULL, "cannot create temp rules file");
    fputs("rules:\n"
          "  - agent: \"coding_v1\"\n"
          "    tool: \"fs_read\"\n"
          "    effect: \"allow\"\n"
          "  - agent: \"coding_v1\"\n"
          "    tool: \"shell_run\"\n"
          "    effect: \"deny\"\n",
          fp);
    fclose(fp);

    daemon_security_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sanitize_level = SANITIZE_LEVEL_HIGH;
    cfg.enable_permission_cache = true;
    cfg.enable_vault = true;
    cfg.enable_audit_logging = true;
    cfg.permission_rules_path = path;

    airy_err_t err;
    memset(&err, 0, sizeof(err));
    int ret = daemon_security_init(&cfg, &err);
    ASSERT(ret == 0, "init with rules file should succeed");

    ASSERT(daemon_check_tool_permission("coding_v1", "fs_read", "execute") == AIRY_OK,
           "granted tool should be allowed");
    ASSERT(daemon_check_tool_permission("coding_v1", "shell_run", "execute") != AIRY_OK,
           "denied tool should be blocked");
    ASSERT(daemon_check_tool_permission("coding_v1", "web_search", "execute") != AIRY_OK,
           "unlisted tool should be denied (fail-closed)");

    daemon_security_shutdown();
    remove(path);
    PASS();
}

int main(void)
{
    printf("\n=== Daemon Security Module Unit Tests ===\n\n");

    test_init_null_config();
    test_double_init();
    test_shutdown_without_init();
    test_sanitize_llm_input_normal();
    test_sanitize_llm_input_dangerous();
    test_sanitize_llm_input_null_params();
    test_sanitize_tool_params_normal();
    test_sanitize_tool_params_dangerous();
    test_sanitize_tool_params_null();
    test_check_tool_permission_no_acl();
    test_check_tool_permission_null_params();
    test_audit_log_event();
    test_audit_log_event_null_params();
    test_sanitize_uninitialized_fail_safe();
    test_init_with_config();
    test_sanitize_llm_input_special_chars();
    test_rules_file_loading();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
