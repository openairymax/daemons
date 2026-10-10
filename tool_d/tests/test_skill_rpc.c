// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_skill_rpc.c
 * @brief skill.* RPC 服务面与技能机制往返单元测试
 *
 * 覆盖：skill_rpc_register 方法族登记（重复注册冲突即已登记证据）、
 * syscall 技能机制往返（custom 安装 → 未注册执行器错误路径 →
 * reg_exec 注册后正路径 → 列举 → 卸载 → ENOENT）。全程无网络依赖
 * （不走 web_search 等内建 HTTP 执行器）。
 */

#include "rpc/skill_rpc.h"

#include "airy_memory.h"
#include "method_dispatcher.h"
#include "syscalls.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_exec_hits = 0;

static airy_err_t test_exec(const char *skill_id, const char *input,
                            size_t input_len, char **out_output)
{
    (void)input_len;
    g_exec_hits++;
    size_t cap = strlen(skill_id) + strlen(input) + 64;
    char *out = (char *)AIRY_MALLOC(cap);
    if (!out)
        return AIRY_ENOMEM;
    snprintf(out, cap, "{\"status\":\"ok\",\"skill_id\":\"%s\",\"echo\":\"%s\"}",
             skill_id, input);
    *out_output = out;
    return AIRY_SUCCESS;
}

static void test_dup_stub(cJSON *params, int id, void *user_data)
{
    (void)params;
    (void)id;
    (void)user_data;
}

/**
 * @brief skill.* 四方法登记到 dispatcher；重复注册被拒即已登记证据
 */
static void test_rpc_register(void)
{
    printf("  test_rpc_register...\n");

    method_dispatcher_t *d = method_dispatcher_create(16);
    assert(d != NULL);

    skill_rpc_register(d);

    int dup = method_dispatcher_register(d, "skill.list", test_dup_stub, NULL);
    assert(dup != 0);

    method_dispatcher_destroy(d);

    printf("    PASSED\n");
}

/**
 * @brief 技能机制往返：custom 安装/执行/列举/卸载全链
 */
static void test_skill_roundtrip(void)
{
    printf("  test_skill_roundtrip...\n");

    const char *url = "file:///opt/skills/greet-custom.py";
    char *skill_id = NULL;
    airy_err_t err = airy_sys_skill_install(url, &skill_id);
    assert(err == AIRY_SUCCESS && skill_id != NULL);

    /* 未注册执行器：custom 管线返回结构化错误 JSON（AIRY_SUCCESS） */
    char *out = NULL;
    err = airy_sys_skill_execute(skill_id, "hello", &out);
    assert(err == AIRY_SUCCESS && out != NULL);
    assert(strstr(out, "no_executor_registered") != NULL);
    AIRY_FREE(out);

    /* URL 以 .py 结尾 → 技能类型解析为 "py"，注册同类型执行器后走正路径 */
    err = airy_sys_skill_reg_exec("py", test_exec);
    assert(err == AIRY_SUCCESS);

    err = airy_sys_skill_execute(skill_id, "hello", &out);
    assert(err == AIRY_SUCCESS && out != NULL);
    assert(strstr(out, "\"status\":\"ok\"") != NULL);
    assert(g_exec_hits == 1);
    AIRY_FREE(out);

    /* 列举含已安装技能 */
    char **skills = NULL;
    size_t count = 0;
    err = airy_sys_skill_list(&skills, &count);
    assert(err == AIRY_SUCCESS && count >= 1);
    int found = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(skills[i], skill_id) == 0)
            found = 1;
        AIRY_FREE(skills[i]);
    }
    AIRY_FREE(skills);
    assert(found);

    /* 卸载后执行返回 ENOENT */
    err = airy_sys_skill_uninstall(skill_id);
    assert(err == AIRY_SUCCESS);
    err = airy_sys_skill_execute(skill_id, "hello", &out);
    assert(err == AIRY_ENOENT);

    AIRY_FREE(skill_id);

    printf("    PASSED\n");
}

int main(void)
{
    printf("test_skill_rpc:\n");
    test_rpc_register();
    test_skill_roundtrip();
    printf("All skill_rpc tests PASSED\n");
    return 0;
}
