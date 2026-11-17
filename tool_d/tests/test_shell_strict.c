// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_shell_strict.c
 * @brief C-24 shell 执行语义测试：默认 set -e + pipefail、退出码透传、
 *        per-tool shell_strict=0 回退旧语义、非法取值按严格处理。
 *
 * 三条判据直接落在 POSIX sh 的确定性行为上，不依赖网络、root 或外部包：
 * `false | true` 必失败、`true | false` 必失败、`false || true` 必成功。
 * Windows 的 cmd.exe 没有 pipefail，仅验证退出码透传与 strict 为 no-op。
 */

#include "builtin/builtin_exec_internal.h"

#include "airy_memory.h"
#include "error_codes.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void run(const char *json, tool_result_t *res)
{
    AIRY_MEMSET(res, 0, sizeof(*res));
    assert(shell_run_tool(json, 5000, res) == AIRY_OK);
}

static void drop(tool_result_t *res)
{
    AIRY_FREE(res->output);
    AIRY_FREE(res->error);
    AIRY_MEMSET(res, 0, sizeof(*res));
}

#ifndef _WIN32

static void test_ok(void)
{
    tool_result_t res;
    run("{\"command\":\"echo hello\"}", &res);
    assert(res.err_code == AIRY_OK);
    assert(res.exit_code == 0);
    assert(res.output && strstr(res.output, "hello"));
    drop(&res);
    printf("PASS ok\n");
}

/* 判据一：管道首段失败必须整体失败（无 pipefail 时 sh 返回 0）。 */
static void test_pipe_head(void)
{
    tool_result_t res;
    run("{\"command\":\"false | true\"}", &res);
    assert(res.exit_code != 0);
    assert(res.err_code != AIRY_OK);
    drop(&res);
    printf("PASS pipe_head\n");
}

/* 判据二：管道尾段失败必须整体失败。 */
static void test_pipe_tail(void)
{
    tool_result_t res;
    run("{\"command\":\"true | false\"}", &res);
    assert(res.exit_code != 0);
    drop(&res);
    printf("PASS pipe_tail\n");
}

/* 判据三：或列表显式吞掉失败，整体必须成功。 */
static void test_or_list(void)
{
    tool_result_t res;
    run("{\"command\":\"false || true\"}", &res);
    assert(res.err_code == AIRY_OK);
    assert(res.exit_code == 0);
    drop(&res);
    printf("PASS or_list\n");
}

/* set -e：首条失败即终止，后续命令不得执行。 */
static void test_errexit(void)
{
    tool_result_t res;
    run("{\"command\":\"false; echo continued\"}", &res);
    assert(res.exit_code != 0);
    assert(!res.output || !strstr(res.output, "continued"));
    drop(&res);
    printf("PASS errexit\n");
}

/* 退出码透传：数字原样回给调用方。 */
static void test_exit_code(void)
{
    tool_result_t res;
    run("{\"command\":\"exit 7\"}", &res);
    assert(res.exit_code == 7);
    assert(res.err_code != AIRY_OK);
    drop(&res);
    printf("PASS exit_code\n");
}

/* 回退：shell_strict=0（数字、布尔、字符串三种写法）恢复旧语义。 */
static void test_strict_off(void)
{
    static const char *jsons[] = {
        "{\"command\":\"false | true\",\"shell_strict\":0}",
        "{\"command\":\"false | true\",\"shell_strict\":false}",
        "{\"command\":\"false | true\",\"shell_strict\":\"0\"}",
        "{\"command\":\"false | true\",\"shell_strict\":\"off\"}",
        NULL};
    for (int i = 0; jsons[i]; i++) {
        tool_result_t res;
        run(jsons[i], &res);
        assert(res.err_code == AIRY_OK);
        assert(res.exit_code == 0);
        drop(&res);
    }
    printf("PASS strict_off\n");
}

/* 显式 true 与非法取值：都按严格处理，不得静默放宽。 */
static void test_strict_on(void)
{
    static const char *jsons[] = {
        "{\"command\":\"false | true\",\"shell_strict\":true}",
        "{\"command\":\"false | true\",\"shell_strict\":1}",
        "{\"command\":\"false | true\",\"shell_strict\":\"maybe\"}",
        NULL};
    for (int i = 0; jsons[i]; i++) {
        tool_result_t res;
        run(jsons[i], &res);
        assert(res.exit_code != 0);
        drop(&res);
    }
    printf("PASS strict_on\n");
}

int main(void)
{
    test_ok();
    test_pipe_head();
    test_pipe_tail();
    test_or_list();
    test_errexit();
    test_exit_code();
    test_strict_off();
    test_strict_on();
    printf("ALL PASS test_shell_strict\n");
    return 0;
}

#else /* _WIN32 */

/* Windows：cmd.exe 无 set -e / pipefail，strict 为 no-op；只验证退出码透传。 */
static void test_passthrough(void)
{
    tool_result_t res;
    run("{\"command\":\"exit /b 7\"}", &res);
    assert(res.exit_code == 7);
    drop(&res);
    run("{\"command\":\"exit /b 7\",\"shell_strict\":false}", &res);
    assert(res.exit_code == 7);
    drop(&res);
    printf("PASS passthrough\n");
}

int main(void)
{
    test_passthrough();
    printf("ALL PASS test_shell_strict\n");
    return 0;
}

#endif /* _WIN32 */
