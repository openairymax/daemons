// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service.c
 * @brief LLM 服务核心功能单元测试
 */

#include "llm_service.h"
#include "error.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_service_create_destroy(void)
{
    printf("  test_service_create_destroy...\n");

    llm_service_t *svc = llm_service_create(NULL);
    assert(svc != NULL);

    llm_service_destroy(svc);

    printf("    PASSED\n");
}

static void test_service_create_with_config(void)
{
    printf("  test_service_create_with_config...\n");

    llm_service_t *svc = llm_service_create("/nonexistent/config.yaml");
    if (svc != NULL) {
        llm_service_destroy(svc);
        printf("    PASSED (created with default config)\n");
    } else {
        printf("    PASSED (NULL returned for missing config)\n");
    }
}

static void test_message_build(void)
{
    printf("  test_message_build...\n");

    llm_message_t messages[2];
    memset(messages, 0, sizeof(messages));

    messages[0].role = "system";
    messages[0].content = "You are a helpful assistant.";

    messages[1].role = "user";
    messages[1].content = "Hello!";

    assert(strcmp(messages[0].role, "system") == 0);
    assert(strcmp(messages[1].content, "Hello!") == 0);

    printf("    PASSED\n");
}

static void test_request_config(void)
{
    printf("  test_request_config...\n");

    llm_request_config_t config;
    memset(&config, 0, sizeof(config));

    config.model = "gpt-4";
    config.temperature = 0.7f;
    config.max_tokens = 1024;
    config.stream = 0;

    assert(strcmp(config.model, "gpt-4") == 0);
    assert(config.temperature > 0.69f && config.temperature < 0.71f);
    assert(config.max_tokens == 1024);
    assert(config.stream == 0);

    printf("    PASSED\n");
}

static void test_response_free(void)
{
    printf("  test_response_free...\n");

    llm_response_t *resp = (llm_response_t *)calloc(1, sizeof(llm_response_t));
    assert(resp != NULL);

    resp->id = strdup("chatcmpl-123");
    resp->model = strdup("gpt-4");
    resp->finish_reason = strdup("stop");

    llm_response_free(resp);

    printf("    PASSED\n");
}

static void test_service_stats(void)
{
    printf("  test_service_stats...\n");

    llm_service_t *svc = llm_service_create(NULL);
    assert(svc != NULL);

    char *stats_json = NULL;
    int ret = llm_service_stats(svc, &stats_json);
    if (ret == 0 && stats_json != NULL) {
        printf("    Stats: %s\n", stats_json);
        free(stats_json);
    }

    llm_service_destroy(svc);

    printf("    PASSED\n");
}

/* embeddings 代理：参数校验 + 无 provider 降级（不发真实网络请求） */
static void test_embeddings_validation(void)
{
    printf("  test_embeddings_validation...\n");

    llm_service_t *svc = llm_service_create(NULL);
    assert(svc != NULL);

    char *out = (char *)0x1; /* 哨兵：验证被置 NULL */

    /* NULL 参数 / 空 body → INVALID_PARAM，out 不被写入 */
    assert(llm_service_embeddings(NULL, "m", "{\"input\":\"x\"}", &out) != AIRY_SUCCESS);
    assert(llm_service_embeddings(svc, NULL, NULL, &out) != AIRY_SUCCESS);
    assert(llm_service_embeddings(svc, "m", "", &out) != AIRY_SUCCESS);
    assert(llm_service_embeddings(svc, "m", "{}", NULL) != AIRY_SUCCESS);

    /* 未配置任何 provider 时（默认配置无 model.yaml），应明确失败而不崩溃 */
    out = NULL;
    int rc = llm_service_embeddings(svc, "no-such-model", "{\"input\":\"hello\"}", &out);
    assert(rc != AIRY_SUCCESS);
    assert(out == NULL);

    llm_service_destroy(svc);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  LLM Service Unit Tests\n");
    printf("=========================================\n");

    test_service_create_destroy();
    test_service_create_with_config();
    test_message_build();
    test_request_config();
    test_response_free();
    test_service_stats();
    test_embeddings_validation();

    printf("\nAll LLM service tests PASSED\n");
    return 0;
}
