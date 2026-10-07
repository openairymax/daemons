// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service.c
 * @brief A2A 服务单元测试
 *
 * 覆盖服务核经 proto_registry 注入式消费适配器的完整路径：装配/销毁、
 * 注册/注销/发现、卡片查询、任务创建/查询、消息发送与计数。
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

static cJSON *A2A_PARSE(char *json)
{
    cJSON *obj = cJSON_Parse(json);
    a2a_result_free(json);
    return obj;
}

static void test_create_destroy(void)
{
    printf("  test_create_destroy...\n");

    a2a_service_t *svc = a2a_service_create();
    assert(svc != NULL);

    size_t agents = 1, tasks = 1;
    assert(a2a_service_stats(svc, &agents, &tasks) == AIRY_SUCCESS);
    assert(agents == 0);
    assert(tasks == 0);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_register_and_discover(void)
{
    printf("  test_register_and_discover...\n");

    a2a_service_t *svc = a2a_service_create();
    assert(svc != NULL);

    char *result = NULL;
    int ret = a2a_service_register_agent(svc, A2A_CARD_ALPHA, &result);
    assert(ret == AIRY_SUCCESS);
    cJSON *reg = A2A_PARSE(result);
    assert(reg != NULL);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(reg, "registered")));
    cJSON *rid = cJSON_GetObjectItem(reg, "agent_id");
    assert(rid != NULL && cJSON_IsString(rid));
    assert(strcmp(rid->valuestring, "agent_alpha") == 0);
    cJSON_Delete(reg);

    assert(a2a_service_register_agent(svc, A2A_CARD_BETA, &result) == AIRY_SUCCESS);
    a2a_result_free(result);

    size_t agent_count = 0;
    assert(a2a_service_stats(svc, &agent_count, NULL) == AIRY_SUCCESS);
    assert(agent_count == 2);

    result = NULL;
    ret = a2a_service_discover_agents(svc, NULL, NULL, &result);
    assert(ret == AIRY_SUCCESS);
    cJSON *disc = A2A_PARSE(result);
    assert(disc != NULL);
    cJSON *arr = cJSON_GetObjectItem(disc, "agents");
    cJSON *cnt = cJSON_GetObjectItem(disc, "count");
    assert(arr != NULL && cJSON_IsArray(arr));
    assert(cJSON_GetArraySize(arr) == 2);
    assert(cnt != NULL && cJSON_IsNumber(cnt) && (size_t)cnt->valuedouble == 2);
    cJSON_Delete(disc);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_unregister(void)
{
    printf("  test_unregister...\n");

    a2a_service_t *svc = a2a_service_create();
    assert(svc != NULL);

    char *result = NULL;
    assert(a2a_service_register_agent(svc, A2A_CARD_ALPHA, &result) == AIRY_SUCCESS);
    a2a_result_free(result);

    result = NULL;
    int ret = a2a_service_unregister_agent(svc, "agent_alpha", &result);
    assert(ret == AIRY_SUCCESS);
    cJSON *unreg = A2A_PARSE(result);
    assert(unreg != NULL);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(unreg, "unregistered")));
    cJSON_Delete(unreg);

    size_t agent_count = 1;
    assert(a2a_service_stats(svc, &agent_count, NULL) == AIRY_SUCCESS);
    assert(agent_count == 0);

    result = NULL;
    ret = a2a_service_unregister_agent(svc, "agent_alpha", &result);
    assert(ret != AIRY_SUCCESS);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_get_agent_card(void)
{
    printf("  test_get_agent_card...\n");

    a2a_service_t *svc = a2a_service_create();
    assert(svc != NULL);

    char *result = NULL;
    assert(a2a_service_register_agent(svc, A2A_CARD_ALPHA, &result) == AIRY_SUCCESS);
    a2a_result_free(result);

    result = NULL;
    int ret = a2a_service_get_agent_card(svc, "agent_alpha", &result);
    assert(ret == AIRY_SUCCESS);
    cJSON *root = A2A_PARSE(result);
    assert(root != NULL);
    cJSON *card = cJSON_GetObjectItem(root, "agent");
    assert(card != NULL && cJSON_IsObject(card));
    cJSON *id = cJSON_GetObjectItem(card, "id");
    assert(id != NULL && cJSON_IsString(id));
    assert(strcmp(id->valuestring, "agent_alpha") == 0);
    cJSON_Delete(root);

    result = NULL;
    ret = a2a_service_get_agent_card(svc, "nonexistent", &result);
    assert(ret != AIRY_SUCCESS);
    assert(result == NULL);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_create_task(void)
{
    printf("  test_create_task...\n");

    a2a_service_t *svc = a2a_service_create();
    assert(svc != NULL);

    char *result = NULL;
    assert(a2a_service_register_agent(svc, A2A_CARD_ALPHA, &result) == AIRY_SUCCESS);
    a2a_result_free(result);

    result = NULL;
    int ret = a2a_service_create_task(svc, "agent_alpha", "summarize document",
                                      "{\"text\":\"hello\"}", &result);
    assert(ret == AIRY_SUCCESS);
    cJSON *root = A2A_PARSE(result);
    assert(root != NULL);
    cJSON *task = cJSON_GetObjectItem(root, "task");
    assert(task != NULL && cJSON_IsObject(task));
    cJSON *agent_id = cJSON_GetObjectItem(task, "agent_id");
    assert(agent_id != NULL && cJSON_IsString(agent_id));
    assert(strcmp(agent_id->valuestring, "agent_alpha") == 0);
    cJSON_Delete(root);

    size_t task_count = 0;
    assert(a2a_service_stats(svc, NULL, &task_count) == AIRY_SUCCESS);
    assert(task_count == 1);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_send_message(void)
{
    printf("  test_send_message...\n");

    a2a_service_t *svc = a2a_service_create();
    assert(svc != NULL);

    char *result = NULL;
    assert(a2a_service_register_agent(svc, A2A_CARD_ALPHA, &result) == AIRY_SUCCESS);
    a2a_result_free(result);
    assert(a2a_service_register_agent(svc, A2A_CARD_BETA, &result) == AIRY_SUCCESS);
    a2a_result_free(result);

    result = NULL;
    int ret = a2a_service_send_message(svc, "agent_beta", "user", "{\"prompt\":\"hi\"}", &result);
    assert(ret == AIRY_SUCCESS);
    cJSON *root = A2A_PARSE(result);
    assert(root != NULL);
    cJSON *arr = cJSON_GetObjectItem(root, "responses");
    cJSON *cnt = cJSON_GetObjectItem(root, "count");
    assert(arr != NULL && cJSON_IsArray(arr));
    assert(cnt != NULL && cJSON_IsNumber(cnt) && (size_t)cnt->valuedouble >= 1);
    cJSON_Delete(root);

    a2a_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_stats(void)
{
    printf("  test_stats...\n");

    a2a_service_t *svc = a2a_service_create();
    assert(svc != NULL);

    char *result = NULL;
    assert(a2a_service_register_agent(svc, A2A_CARD_ALPHA, &result) == AIRY_SUCCESS);
    a2a_result_free(result);
    assert(a2a_service_register_agent(svc, A2A_CARD_BETA, &result) == AIRY_SUCCESS);
    a2a_result_free(result);

    size_t agents = 0, tasks = 0;
    assert(a2a_service_stats(svc, &agents, &tasks) == AIRY_SUCCESS);
    assert(agents == 2);
    assert(tasks == 0);

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
    test_stats();
    printf("=== All tests PASSED ===\n");
    return 0;
}
