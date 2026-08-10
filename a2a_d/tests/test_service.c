// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file test_service.c
 * @brief A2A 服务单元测试
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#include "a2a_service.h"

#include "airy_memory.h"

#include <assert.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *A2A_CARD_ALPHA =
    "{\"id\":\"agent_alpha\",\"name\":\"Alpha\",\"description\":\"alpha agent\","
    "\"url\":\"http://alpha.local\",\"version\":\"1.0.0\","
    "\"protocol_version\":3,\"capabilities\":1,\"available\":true}";

static const char *A2A_CARD_BETA =
    "{\"id\":\"agent_beta\",\"name\":\"Beta\",\"description\":\"beta agent\","
    "\"url\":\"http://beta.local\",\"version\":\"1.0.0\","
    "\"protocol_version\":3,\"capabilities\":2,\"available\":true}";

static void test_create_destroy(void)
{
    printf("  test_create_destroy...\n");

    a2a_service_t *svc = a2a_service_create(0, 0);
    assert(svc != NULL);
    assert(a2a_service_count(svc) == 0);
    assert(a2a_service_task_count(svc) == 0);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_register_and_discover(void)
{
    printf("  test_register_and_discover...\n");

    a2a_service_t *svc = a2a_service_create(16, 32);
    assert(svc != NULL);

    int ret = a2a_service_register_agent(svc, A2A_CARD_ALPHA);
    assert(ret == AIRY_SUCCESS);
    ret = a2a_service_register_agent(svc, A2A_CARD_BETA);
    assert(ret == AIRY_SUCCESS);

    assert(a2a_service_count(svc) == 2);

    /* discover 无过滤条件应返回 2 个可用智能体 */
    char *results_json = NULL;
    size_t count = 0;
    ret = a2a_service_discover_agents(svc, NULL, NULL, &results_json, &count);
    assert(ret == AIRY_SUCCESS);
    assert(count == 2);
    assert(results_json != NULL);

    /* 验证返回的是合法 JSON 数组 */
    cJSON *arr = cJSON_Parse(results_json);
    assert(arr != NULL);
    assert(cJSON_IsArray(arr));
    assert(cJSON_GetArraySize(arr) == 2);
    cJSON_Delete(arr);
    a2a_service_results_free(results_json);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_unregister(void)
{
    printf("  test_unregister...\n");

    a2a_service_t *svc = a2a_service_create(8, 16);
    assert(svc != NULL);

    assert(a2a_service_register_agent(svc, A2A_CARD_ALPHA) == AIRY_SUCCESS);
    assert(a2a_service_count(svc) == 1);

    int ret = a2a_service_unregister_agent(svc, "agent_alpha");
    assert(ret == AIRY_SUCCESS);
    assert(a2a_service_count(svc) == 0);

    /* 注销已不存在的智能体应失败 */
    ret = a2a_service_unregister_agent(svc, "agent_alpha");
    assert(ret != AIRY_SUCCESS);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_get_agent_card(void)
{
    printf("  test_get_agent_card...\n");

    a2a_service_t *svc = a2a_service_create(8, 16);
    assert(svc != NULL);

    assert(a2a_service_register_agent(svc, A2A_CARD_ALPHA) == AIRY_SUCCESS);

    char *card_json = NULL;
    int ret = a2a_service_get_agent_card(svc, "agent_alpha", &card_json);
    assert(ret == AIRY_SUCCESS);
    assert(card_json != NULL);

    /* 验证 JSON 包含 id 字段且值为 agent_alpha */
    cJSON *card = cJSON_Parse(card_json);
    assert(card != NULL);
    cJSON *id = cJSON_GetObjectItem(card, "id");
    assert(id != NULL && cJSON_IsString(id));
    assert(strcmp(id->valuestring, "agent_alpha") == 0);
    cJSON_Delete(card);
    a2a_service_card_free(card_json);

    /* 获取不存在的卡片应返回 NOT_FOUND */
    ret = a2a_service_get_agent_card(svc, "nonexistent", &card_json);
    assert(ret != AIRY_SUCCESS);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_create_task(void)
{
    printf("  test_create_task...\n");

    a2a_service_t *svc = a2a_service_create(8, 16);
    assert(svc != NULL);

    assert(a2a_service_register_agent(svc, A2A_CARD_ALPHA) == AIRY_SUCCESS);

    char *task_json = NULL;
    int ret = a2a_service_create_task(svc, "agent_alpha",
                                        "summarize document",
                                        "{\"text\":\"hello\"}", &task_json);
    assert(ret == AIRY_SUCCESS);
    assert(task_json != NULL);
    assert(a2a_service_task_count(svc) == 1);

    /* 验证任务 JSON 包含 agent_id 字段 */
    cJSON *task = cJSON_Parse(task_json);
    assert(task != NULL);
    cJSON *agent_id = cJSON_GetObjectItem(task, "agent_id");
    assert(agent_id != NULL && cJSON_IsString(agent_id));
    assert(strcmp(agent_id->valuestring, "agent_alpha") == 0);
    cJSON_Delete(task);
    a2a_service_task_free(task_json);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_send_message(void)
{
    printf("  test_send_message...\n");

    a2a_service_t *svc = a2a_service_create(8, 16);
    assert(svc != NULL);

    assert(a2a_service_register_agent(svc, A2A_CARD_ALPHA) == AIRY_SUCCESS);
    assert(a2a_service_register_agent(svc, A2A_CARD_BETA) == AIRY_SUCCESS);

    char *response_json = NULL;
    size_t response_count = 0;
    int ret = a2a_service_send_message(svc, "agent_beta", "user",
                                         "{\"prompt\":\"hi\"}",
                                         &response_json, &response_count);
    assert(ret == AIRY_SUCCESS);
    assert(response_json != NULL);
    assert(response_count >= 1);

    /* 验证响应是合法 JSON 数组 */
    cJSON *arr = cJSON_Parse(response_json);
    assert(arr != NULL);
    assert(cJSON_IsArray(arr));
    cJSON_Delete(arr);
    a2a_service_results_free(response_json);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_count(void)
{
    printf("  test_count...\n");

    a2a_service_t *svc = a2a_service_create(8, 16);
    assert(svc != NULL);

    assert(a2a_service_register_agent(svc, A2A_CARD_ALPHA) == AIRY_SUCCESS);
    assert(a2a_service_register_agent(svc, A2A_CARD_BETA) == AIRY_SUCCESS);

    assert(a2a_service_count(svc) == 2);
    assert(a2a_service_task_count(svc) == 0);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=== A2A Service Unit Tests ===\n");
    test_create_destroy();
    test_register_and_discover();
    test_unregister();
    test_get_agent_card();
    test_create_task();
    test_send_message();
    test_count();
    printf("=== All tests PASSED ===\n");
    return 0;
}
