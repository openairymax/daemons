// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file test_service.c
 * @brief Agent 服务单元测试
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "agent_service.h"

#include "airy_memory.h"

#include <assert.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_create_destroy(void)
{
    printf("  test_create_destroy...\n");

    agent_service_t *svc = agent_service_create(0);
    assert(svc != NULL);
    assert(agent_service_count(svc) == 0);

    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_spawn_and_list(void)
{
    printf("  test_spawn_and_list...\n");

    agent_service_t *svc = agent_service_create(16);
    assert(svc != NULL);

    const char *spec = "{\"type\":\"echo\",\"model\":\"gpt-4\"}";
    char *agent_id = NULL;
    int ret = agent_service_spawn(svc, spec, &agent_id);
    assert(ret == AIRY_SUCCESS);
    assert(agent_id != NULL);
    assert(strlen(agent_id) == 32);

    assert(agent_service_count(svc) == 1);

    char **ids = NULL;
    size_t count = 0;
    ret = agent_service_list(svc, &ids, &count);
    assert(ret == AIRY_SUCCESS);
    assert(count == 1);
    assert(ids != NULL);
    assert(ids[0] != NULL);
    assert(strcmp(ids[0], agent_id) == 0);

    agent_service_list_free(ids, count);
    AIRY_FREE(agent_id);

    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_terminate(void)
{
    printf("  test_terminate...\n");

    agent_service_t *svc = agent_service_create(8);
    assert(svc != NULL);

    char *agent_id = NULL;
    int ret = agent_service_spawn(svc, "{\"type\":\"worker\"}", &agent_id);
    assert(ret == AIRY_SUCCESS);
    assert(agent_id != NULL);

    /* 终止后通过 invoke 验证状态：应返回错误 JSON */
    ret = agent_service_terminate(svc, agent_id);
    assert(ret == AIRY_SUCCESS);

    /* terminate 不回收槽位，count 仍为 1 */
    assert(agent_service_count(svc) == 1);

    char *out_output = NULL;
    ret = agent_service_invoke(svc, agent_id, "ping", 4, &out_output);
    assert(ret != AIRY_SUCCESS);
    assert(out_output != NULL);
    assert(strstr(out_output, "error") != NULL);
    AIRY_FREE(out_output);

    AIRY_FREE(agent_id);
    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_invoke(void)
{
    printf("  test_invoke...\n");

    agent_service_t *svc = agent_service_create(8);
    assert(svc != NULL);

    char *agent_id = NULL;
    int ret = agent_service_spawn(svc, "{\"type\":\"echo\"}", &agent_id);
    assert(ret == AIRY_SUCCESS);
    assert(agent_id != NULL);

    char *out_output = NULL;
    /* P0-2：AIRY_AGENT_NO_SPAWN 模式下无子进程，invoke 必须返回明确
     * 错误，而非"invocation processed"假成功 */
    ret = agent_service_invoke(svc, agent_id, "hello", 5, &out_output);
    assert(ret != AIRY_SUCCESS);
    assert(out_output != NULL);
    assert(strstr(out_output, "error") != NULL);

    AIRY_FREE(out_output);
    AIRY_FREE(agent_id);
    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_invoke_nonexistent(void)
{
    printf("  test_invoke_nonexistent...\n");

    agent_service_t *svc = agent_service_create(8);
    assert(svc != NULL);

    char *out_output = NULL;
    int ret = agent_service_invoke(svc, "nonexistent_agent_id", "hi", 2, &out_output);
    assert(ret == AIRY_ERR_NOT_FOUND);
    assert(out_output != NULL);
    assert(strstr(out_output, "Agent not found") != NULL);
    AIRY_FREE(out_output);

    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_terminate_nonexistent(void)
{
    printf("  test_terminate_nonexistent...\n");

    agent_service_t *svc = agent_service_create(8);
    assert(svc != NULL);

    int ret = agent_service_terminate(svc, "nonexistent_agent_id");
    assert(ret == AIRY_ERR_NOT_FOUND);

    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_capacity_limit(void)
{
    printf("  test_capacity_limit...\n");

    agent_service_t *svc = agent_service_create(2);
    assert(svc != NULL);

    char *r1 = NULL, *r2 = NULL, *r3 = NULL;
    assert(agent_service_spawn(svc, "{\"n\":1}", &r1) == AIRY_SUCCESS);
    assert(agent_service_spawn(svc, "{\"n\":2}", &r2) == AIRY_SUCCESS);
    /* 第三次派生应因容量上限失败 */
    int ret = agent_service_spawn(svc, "{\"n\":3}", &r3);
    assert(ret != AIRY_SUCCESS);
    assert(r3 == NULL);

    AIRY_FREE(r1);
    AIRY_FREE(r2);

    agent_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_spawn_after_terminate(void)
{
    printf("  test_spawn_after_terminate...\n");

    /* 实现不压缩数组（terminate 仅置 status=3），故 max_agents=3
     * 以允许终止后再派生：spawn 2（count=2），terminate 1（count=2），
     * spawn 1（count=3）应成功 */
    agent_service_t *svc = agent_service_create(3);
    assert(svc != NULL);

    char *r1 = NULL, *r2 = NULL;
    assert(agent_service_spawn(svc, "{\"n\":1}", &r1) == AIRY_SUCCESS);
    assert(agent_service_spawn(svc, "{\"n\":2}", &r2) == AIRY_SUCCESS);
    assert(agent_service_count(svc) == 2);

    /* 终止第一个 agent，槽位不回收 */
    assert(agent_service_terminate(svc, r1) == AIRY_SUCCESS);
    assert(agent_service_count(svc) == 2);

    /* 再派生一个：count=2 < max_agents=3，应成功 */
    char *r3 = NULL;
    int ret = agent_service_spawn(svc, "{\"n\":3}", &r3);
    assert(ret == AIRY_SUCCESS);
    assert(r3 != NULL);
    assert(agent_service_count(svc) == 3);

    /* r2 仍可调用：no-spawn 模式下返回明确错误（P0-2） */
    char *out_output = NULL;
    assert(agent_service_invoke(svc, r2, "hi", 2, &out_output) != AIRY_SUCCESS);
    AIRY_FREE(out_output);

    AIRY_FREE(r1);
    AIRY_FREE(r2);
    AIRY_FREE(r3);
    agent_service_destroy(svc);

    printf("    PASSED\n");
}

int main(void)
{
    /* P0-2：单元测试使用确定性模式 — 禁止真实 fork Python 子进程，
     * 验证 service 状态机与"无子进程 invoke 返回明确错误"的新契约。 */
    setenv("AIRY_AGENT_NO_SPAWN", "1", 1);

    printf("=== Agent Service Unit Tests ===\n");
    test_create_destroy();
    test_spawn_and_list();
    test_terminate();
    test_invoke();
    test_invoke_nonexistent();
    test_terminate_nonexistent();
    test_capacity_limit();
    test_spawn_after_terminate();
    printf("=== All tests PASSED ===\n");
    return 0;
}
