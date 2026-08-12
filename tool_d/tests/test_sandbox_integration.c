// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_sandbox_integration.c
 * @brief P3.18 (ACC-DT27): tool_d sandbox 集成测试
 *
 * 验证 ACC-DT27 验收标准：
 *   1. sandbox ALLOW 路径 — airy_sandbox_invoke(SYS_TOOL_EXECUTE) 执行 /usr/bin/echo 成功
 *   2. sandbox DENY 路径 — PERM_DENY 规则使工具执行被拒绝（EACCES）
 *   3. sandbox NULL fail-closed — sandbox 句柄为 NULL 时 invoke 返回 EINVAL
 *   4. tool_executor 集成 — tool_executor_create 内部初始化 sandbox 不破坏现有行为
 *
 * 双层 fail-closed 安全架构验证：
 *   - SafetyGuard 审批（approval_ctx）+ Sandbox 拦截（permission/quota/audit）
 *   - sandbox 为 NULL（初始化失败）时 tool_executor_run 拒绝执行任何工具
 *
 * @note 不使用 assert() 执行副作用操作：Release 构建类型定义 NDEBUG，会将
 *       assert(expr) 展开为 ((void)0)，导致 expr 中的函数调用根本不执行。
 *       所有副作用操作必须用显式 if 检查 + TEST_FAIL。详见 project_memory.md
 *       的 assert/NDEBUG heisenbug 教训。
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "airy_sandbox.h"
#include "executor.h"
#include "airy_memory.h"
#include "syscalls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_count = 0;
static int pass_count = 0;

#define TEST_PASS()   \
    do {              \
        pass_count++; \
        test_count++; \
    } while (0)
#define TEST_FAIL(msg)                 \
    do {                               \
        printf("    FAIL: %s\n", msg); \
        test_count++;                  \
    } while (0)

static tool_execute_args_t *make_echo_args(const char *message, uint32_t timeout_ms)
{
    tool_execute_args_t *t = (tool_execute_args_t *)malloc(sizeof(tool_execute_args_t));
    if (!t)
        return NULL;

    /* argv: {"/usr/bin/echo", "<message>", NULL}
     * argv[0] is conventionally the program name; execvp uses the executable
     * field to locate the binary. */
    char **argv = (char **)malloc(3 * sizeof(char *));
    if (!argv) {
        free(t);
        return NULL;
    }
    argv[0] = strdup("/usr/bin/echo");
    argv[1] = strdup(message);
    argv[2] = NULL;
    if (!argv[0] || !argv[1]) {
        free(argv[0]);
        free(argv[1]);
        free(argv);
        free(t);
        return NULL;
    }

    t->executable = "/usr/bin/echo";
    t->argv = (char *const *)argv;
    t->timeout_ms = timeout_ms;
    t->cap_size = 4096;
    t->output_buffer = (char *)malloc(t->cap_size);
    if (!t->output_buffer) {
        free(argv[0]);
        free(argv[1]);
        free(argv);
        free(t);
        return NULL;
    }
    t->output_buffer[0] = '\0';
    t->exec_result = -999;
    return t;
}

static void free_echo_args(tool_execute_args_t *t)
{
    if (!t)
        return;
    if (t->argv) {
        free(t->argv[0]);
        free(t->argv[1]);
        free((void *)t->argv);
    }
    free(t->output_buffer);
    free(t);
}

/* ========== TEST 1: sandbox ALLOW path — echo executes successfully ==========
 *
 * Verifies:
 *   - airy_sandbox_create_default creates the sandbox
 *   - add_rule(SYS_TOOL_EXECUTE, PERM_ALLOW) configures allowance
 *   - invoke runs /usr/bin/echo hello
 *   - returns SUCCESS, exec_result==0 (echo exit code), output contains "hello"
 */
static void test_sandbox_allow_path(void)
{

    if (airy_sandbox_manager_init() != AIRY_SUCCESS) {
        TEST_FAIL("airy_sandbox_manager_init failed");
        return;
    }

    airy_sandbox_t *sb = NULL;
    if (airy_sandbox_create_default("test_allow", "test_owner", &sb) != AIRY_SUCCESS || !sb) {
        TEST_FAIL("airy_sandbox_create_default failed");
        airy_sandbox_manager_destroy();
        return;
    }

    if (airy_sandbox_add_rule(sb, SYS_TOOL_EXECUTE, PERM_ALLOW, NULL) != AIRY_SUCCESS) {
        TEST_FAIL("airy_sandbox_add_rule(ALLOW) failed");
        airy_sandbox_destroy(sb);
        airy_sandbox_manager_destroy();
        return;
    }

    tool_execute_args_t *t = make_echo_args("hello_airymax", 5000);
    if (!t) {
        TEST_FAIL("make_echo_args failed (OOM)");
        airy_sandbox_destroy(sb);
        airy_sandbox_manager_destroy();
        return;
    }

    void *invoke_args[1] = {t};
    void *out_result = NULL;
    airy_err_t rc = airy_sandbox_invoke(sb, SYS_TOOL_EXECUTE, invoke_args, 1, &out_result);

    if (rc != AIRY_SUCCESS) {
        char msg[128];
        snprintf(msg, sizeof(msg), "invoke returned %d (expected SUCCESS=%d)", (int)rc,
                 (int)AIRY_SUCCESS);
        TEST_FAIL(msg);
        free_echo_args(t);
        airy_sandbox_destroy(sb);
        airy_sandbox_manager_destroy();
        return;
    }

    if (t->exec_result != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "exec_result=%d (expected 0)", t->exec_result);
        TEST_FAIL(msg);
        free_echo_args(t);
        airy_sandbox_destroy(sb);
        airy_sandbox_manager_destroy();
        return;
    }

    if (!t->output_buffer || strstr(t->output_buffer, "hello_airymax") == NULL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "output='%s' (expected to contain 'hello_airymax')",
                 t->output_buffer ? t->output_buffer : "(null)");
        TEST_FAIL(msg);
        free_echo_args(t);
        airy_sandbox_destroy(sb);
        airy_sandbox_manager_destroy();
        return;
    }

    (void)out_result;
    printf("    ALLOW path: echo executed via sandbox, output='%s'\n", t->output_buffer);
    free_echo_args(t);
    airy_sandbox_destroy(sb);
    airy_sandbox_manager_destroy();
    TEST_PASS();
}

/* ========== TEST 2: sandbox DENY path — the tool is rejected ==========
 *
 * Verifies:
 *   - add_rule(SYS_TOOL_EXECUTE, PERM_DENY) configures denial
 *   - invoke returns EACCES (permission denied)
 *   - exec_result keeps its sentinel value (the tool did not actually run)
 */
static void test_sandbox_deny_path(void)
{
    if (airy_sandbox_manager_init() != AIRY_SUCCESS) {
        TEST_FAIL("airy_sandbox_manager_init failed");
        return;
    }

    airy_sandbox_t *sb = NULL;
    if (airy_sandbox_create_default("test_deny", "test_owner", &sb) != AIRY_SUCCESS || !sb) {
        TEST_FAIL("airy_sandbox_create_default failed");
        airy_sandbox_manager_destroy();
        return;
    }

    if (airy_sandbox_add_rule(sb, SYS_TOOL_EXECUTE, PERM_DENY, NULL) != AIRY_SUCCESS) {
        TEST_FAIL("airy_sandbox_add_rule(DENY) failed");
        airy_sandbox_destroy(sb);
        airy_sandbox_manager_destroy();
        return;
    }

    tool_execute_args_t *t = make_echo_args("should_not_run", 5000);
    if (!t) {
        TEST_FAIL("make_echo_args failed (OOM)");
        airy_sandbox_destroy(sb);
        airy_sandbox_manager_destroy();
        return;
    }

    void *invoke_args[1] = {t};
    void *out_result = NULL;
    airy_err_t rc = airy_sandbox_invoke(sb, SYS_TOOL_EXECUTE, invoke_args, 1, &out_result);

    if (rc != AIRY_EACCES) {
        char msg[128];
        snprintf(msg, sizeof(msg), "invoke returned %d (expected EACCES=%d)", (int)rc,
                 (int)AIRY_EACCES);
        TEST_FAIL(msg);
        free_echo_args(t);
        airy_sandbox_destroy(sb);
        airy_sandbox_manager_destroy();
        return;
    }

    if (t->exec_result != -999) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "exec_result=%d (expected -999 sentinel, tool should NOT have run)",
                 t->exec_result);
        TEST_FAIL(msg);
        free_echo_args(t);
        airy_sandbox_destroy(sb);
        airy_sandbox_manager_destroy();
        return;
    }

    (void)out_result;
    free_echo_args(t);
    airy_sandbox_destroy(sb);
    airy_sandbox_manager_destroy();
    TEST_PASS();
    printf("    DENY path: tool execution blocked by sandbox (EACCES)\n");
}

/* ========== TEST 3: sandbox NULL fail-closed ==========
 *
 * Verifies that invoke returns EINVAL when the sandbox handle is NULL.
 * This corresponds to the `if (!exec->sandbox) return AIRY_EPERM` defense in
 * tool_executor_run.
 */
static void test_sandbox_null_fail_closed(void)
{
    void *out_result = NULL;

    void *invoke_args[1] = {NULL};
    airy_err_t rc = airy_sandbox_invoke(NULL, SYS_TOOL_EXECUTE, invoke_args, 1, &out_result);

    if (rc != AIRY_EINVAL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "invoke(NULL,...) returned %d (expected EINVAL=%d)", (int)rc,
                 (int)AIRY_EINVAL);
        TEST_FAIL(msg);
        return;
    }

    TEST_PASS();
    printf("    NULL fail-closed: invoke(NULL,...) returned EINVAL\n");
}

/* ========== TEST 4: tool_executor integration — sandbox initialized in
 * create ==========
 *
 * Verifies executor.c's sandbox integration does not break existing behavior:
 *   - tool_executor_create internally calls airy_sandbox_manager_init +
 *     create_default
 *   - without approval_ctx, calling tool_executor_run -> should return EPERM
 *     (approval_ctx NULL fail-closed)
 *   - this confirms the sandbox init code path works and that, in the
 *     two-tier fail-closed design, the approval_ctx layer comes first (denies
 *     first), so the sandbox layer is never reached
 */
static void test_executor_sandbox_integration(void)
{
    tool_executor_t *exec = tool_executor_create(NULL);
    if (!exec) {
        TEST_FAIL("tool_executor_create failed (sandbox init may have crashed)");
        return;
    }

    tool_metadata_t meta;
    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = "test_echo";
    meta.name = "echo_test";
    meta.executable = "/usr/bin/echo";
    meta.timeout_sec = 5;

    tool_result_t *result = NULL;
    int ret = tool_executor_run(exec, &meta, "hello", NULL, &result);

    /* Verifies: unset approval_ctx -> fail-closed denial (EPERM)
     * Note: AIRY_EPERM is the return value in executor.c for a NULL
     * approval_ctx. This confirms sandbox initialization does not bypass
     * approval_ctx's fail-closed behavior. */
    if (ret != AIRY_EPERM) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "tool_executor_run returned %d (expected EPERM=%d, approval_ctx NULL should "
                 "fail-closed)",
                 ret, (int)AIRY_EPERM);
        TEST_FAIL(msg);
        if (result)
            tool_result_free(result);
        tool_executor_destroy(exec);
        return;
    }

    if (result)
        tool_result_free(result);
    tool_executor_destroy(exec);
    TEST_PASS();
    printf("    executor integration: sandbox init OK, approval_ctx NULL fail-closed preserved\n");
}

/* ========== main ========== */
int main(void)
{
    printf("=== P3.18 (ACC-DT27) tool_d sandbox integration tests ===\n\n");

    printf("[Test 1] sandbox ALLOW path (execute /usr/bin/echo)\n");
    test_sandbox_allow_path();

    printf("\n[Test 2] sandbox DENY path (tool blocked)\n");
    test_sandbox_deny_path();

    printf("\n[Test 3] sandbox NULL fail-closed\n");
    test_sandbox_null_fail_closed();

    printf("\n[Test 4] tool_executor sandbox integration (create + fail-closed)\n");
    test_executor_sandbox_integration();

    printf("\n=== Results: %d/%d passed ===\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}
