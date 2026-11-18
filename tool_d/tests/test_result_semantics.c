// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_result_semantics.c
 * @brief C-2 失败语义注入测试（进程退出非 0 / 超时 / 空输出 / 部分输出 /
 *        审批拒绝），断言 err_code 可区分且 tool_result_ok 判据与之一致。
 *
 * 协议错用例由消费侧（atoms tool_svc_adapter）承接，见
 * atoms/coreloopthree/tests/unit/test_tool_result_semantics.c。
 */

#include "core/executor.h"
#include "core/executor_run.h"
#include "core/executor_gate.h"
#include "core/approval_gate.h"
#include "approval/tool_approval.h"
#include "tool_service.h"

#include "airy_memory.h"
#include "daemon_security.h"
#include "error_codes.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 注入放行审批：executor 拥有 gate，destroy 时释放，测试不得重复释放。 */
static void setup_approval(tool_executor_t *exec, const char *tool_name)
{
    daemon_security_init(NULL, NULL);
    int ar = daemon_security_add_acl_rule("tool_d", tool_name, true);

    assert(ar == 0);

    tool_approval_config_t cfg;
    AIRY_MEMSET(&cfg, 0, sizeof(cfg));
    cfg.agent_id = "tool_d";
    cfg.enable_safety_guard_chain = false;
    cfg.enable_audit_logging = false;
    approval_gate_t *gate = gate_create(&cfg);

    assert(gate != NULL);
    exec_set_gate(exec, gate);
}

static tool_result_t *run_tool(tool_executor_t *exec, const char *id, const char *executable,
                               const char *params, int timeout_sec)
{
    tool_metadata_t meta;
    tool_result_t *result = NULL;

    AIRY_MEMSET(&meta, 0, sizeof(meta));
    meta.id = (char *)id;
    meta.name = (char *)id;
    meta.executable = (char *)executable;
    meta.timeout_sec = timeout_sec;

    int rc = tool_executor_run(exec, &meta, params, "tool_d", &result);

    printf("    %-14s rc=%d err_code=%d exit=%d output_len=%zu\n", id, rc,
           result ? (int)result->err_code : 0, result ? result->exit_code : -1,
           (result && result->output) ? strlen(result->output) : (size_t)0);
    assert(result != NULL);
    return result;
}

/* 进程退出非 0：false 退出码 1，必须判失败且给通用业务码。 */
static void test_nonzero_exit(void)
{
    printf("  test_nonzero_exit...\n");

    /* macOS 无 /bin/false（在 /usr/bin），按宿主实际位置取。 */
    const char *false_exe =
        access("/bin/false", X_OK) == 0 ? "/bin/false" : "/usr/bin/false";

    tool_executor_t *exec = tool_executor_create(NULL);

    assert(exec != NULL);
    setup_approval(exec, "nonzero_exit");

    tool_result_t *res = run_tool(exec, "nonzero_exit", false_exe, NULL, 5);

    assert(!tool_result_ok(res));
    assert(res->err_code == AIRY_ERR_GENERIC_FAIL);
    assert(res->exit_code == 1);
    assert(res->error != NULL);
    tool_result_free(res);
    tool_executor_destroy(exec);
    printf("    PASSED\n");
}

/* 超时：预算 1s 跑 /bin/sleep 5，错误码必须是 TIMEOUT 而非通用失败。 */
static void test_timeout(void)
{
    printf("  test_timeout...\n");

    tool_executor_t *exec = tool_executor_create(NULL);

    assert(exec != NULL);
    setup_approval(exec, "timeout_case");

    tool_result_t *res = run_tool(exec, "timeout_case", "/bin/sleep", "5", 1);

    assert(!tool_result_ok(res));
    assert(res->err_code == AIRY_ERR_TIMEOUT);
    tool_result_free(res);
    tool_executor_destroy(exec);
    printf("    PASSED\n");
}

/* 空输出 + 退出 0：成功但正文为空，判据必须看 err_code 而非正文有无。 */
static void test_empty_output(void)
{
    printf("  test_empty_output...\n");

    tool_executor_t *exec = tool_executor_create(NULL);

    assert(exec != NULL);
    setup_approval(exec, "empty_out");

    tool_result_t *res = run_tool(exec, "empty_out", "/bin/true", NULL, 5);

    assert(tool_result_ok(res));
    assert(res->err_code == AIRY_OK);
    assert(res->output != NULL && res->output[0] == '\0');
    tool_result_free(res);
    tool_executor_destroy(exec);
    printf("    PASSED\n");
}

/* 部分输出：先产出正文再以非 0 退出，正文保留且 err_code 判失败。 */
static void test_partial_output(void)
{
    printf("  test_partial_output...\n");

    char path[128];
    snprintf(path, sizeof(path), "/tmp/airy_c2_partial_%d.sh", (int)getpid());

    FILE *fp = fopen(path, "w");

    assert(fp != NULL);
    fputs("#!/bin/sh\necho partial-out\nexit 7\n", fp);
    fclose(fp);
    assert(chmod(path, 0755) == 0);

    tool_executor_t *exec = tool_executor_create(NULL);

    assert(exec != NULL);
    setup_approval(exec, "partial_out");

    tool_result_t *res = run_tool(exec, "partial_out", path, NULL, 5);

    assert(!tool_result_ok(res));
    assert(res->err_code == AIRY_ERR_GENERIC_FAIL);
    assert(res->output != NULL && strstr(res->output, "partial-out") != NULL);
    tool_result_free(res);
    tool_executor_destroy(exec);
    unlink(path);
    printf("    PASSED\n");
}

/* 审批拒绝：未注入 gate 时 fail-closed，错误码必须是权限类而非通用失败。 */
static void test_approval_denied(void)
{
    printf("  test_approval_denied...\n");

    tool_executor_t *exec = tool_executor_create(NULL);

    assert(exec != NULL);

    tool_result_t *res = run_tool(exec, "deny_probe", "/bin/true", NULL, 5);

    assert(!tool_result_ok(res));
    assert(res->err_code == AIRY_ERR_PERMISSION_DENIED);
    assert(res->failure_class == TOOL_RESULT_CLASS_FATAL);
    tool_result_free(res);
    tool_executor_destroy(exec);
    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Tool Result Semantics Tests (C-2)\n");
    printf("=========================================\n");

    test_nonzero_exit();
    test_timeout();
    test_empty_output();
    test_partial_output();
    test_approval_denied();

    printf("\nAll tool result semantics tests PASSED\n");
    return 0;
}
