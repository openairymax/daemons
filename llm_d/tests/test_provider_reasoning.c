// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_provider_reasoning.c
 * @brief reasoning_content 请求/响应单元测试
 *
 * DeepSeek/Kimi 思考模式要求在工具循环中回传 assistant 消息的
 * reasoning_content，否则上游 API 返回 400。本测试覆盖：
 * 1. provider_build_openai_request 在请求体中原样回传 reasoning_content
 * 2. provider_parse_openai_response 从响应中提取 reasoning_content
 */

#include "error.h"
#include "provider.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_build_request_echoes_reasoning(void)
{
    printf("  test_build_request_echoes_reasoning...\n");

    llm_message_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = "user";
    msgs[0].content = "What is 6*7?";
    msgs[1].role = "assistant";
    msgs[1].content = "42";
    msgs[1].reasoning_content = "6*7=42";

    llm_request_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.model = "deepseek-reasoner";
    cfg.messages = msgs;
    cfg.message_count = 2;

    char *body = provider_build_openai_request(&cfg, "deepseek-reasoner");
    assert(body != NULL);
    assert(strstr(body, "reasoning_content") != NULL);
    assert(strstr(body, "6*7=42") != NULL);
    free(body);

    printf("    PASSED\n");
}

static void test_parse_response_reads_reasoning(void)
{
    printf("  test_parse_response_reads_reasoning...\n");

    const char *json = "{"
                       "\"id\":\"chatcmpl-r1\",\"model\":\"deepseek-reasoner\","
                       "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
                       "\"content\":\"42\",\"reasoning_content\":\"6*7=42\"},"
                       "\"finish_reason\":\"stop\"}],"
                       "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,"
                       "\"total_tokens\":15}}";

    llm_response_t *resp = NULL;
    int ret = provider_parse_openai_response(json, &resp);
    assert(ret == AIRY_OK);
    assert(resp != NULL);
    assert(resp->choice_count == 1);
    assert(strcmp(resp->choices[0].content, "42") == 0);
    assert(resp->choices[0].reasoning_content != NULL);
    assert(strcmp(resp->choices[0].reasoning_content, "6*7=42") == 0);

    llm_response_free(resp);
    printf("    PASSED\n");
}

static void test_parse_response_without_reasoning(void)
{
    printf("  test_parse_response_without_reasoning...\n");

    const char *json = "{"
                       "\"id\":\"chatcmpl-r2\",\"model\":\"gpt-4o\","
                       "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
                       "\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
                       "\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":1,"
                       "\"total_tokens\":3}}";

    llm_response_t *resp = NULL;
    int ret = provider_parse_openai_response(json, &resp);
    assert(ret == AIRY_OK);
    assert(resp != NULL);
    assert(resp->choice_count == 1);
    assert(resp->choices[0].reasoning_content == NULL);

    llm_response_free(resp);
    printf("    PASSED\n");
}

static void test_buf_append(void)
{
    printf("  test_buf_append...\n");

    /* Grow from NULL (streaming accumulators start empty). */
    char *buf = NULL;
    size_t cap = 0, len = 0;
    buf = provider_buf_append(buf, &cap, &len, "Let me");
    assert(buf != NULL && len == 6);
    buf = provider_buf_append(buf, &cap, &len, " verify");
    assert(len == 13);
    assert(memcmp(buf, "Let me verify", 13) == 0);

    /* NULL / empty appends are no-ops. */
    char *b2 = provider_buf_append(buf, &cap, &len, NULL);
    assert(b2 == buf && len == 13);
    b2 = provider_buf_append(buf, &cap, &len, "");
    assert(b2 == buf && len == 13);

    /* Force growth across realloc with many small chunks. */
    for (int i = 0; i < 500; i++) {
        char *grown = provider_buf_append(buf, &cap, &len, "x");
        assert(grown != NULL);
        buf = grown;
    }
    assert(len == 13 + 500);
    assert(buf[len] == '\0');

    free(buf);
    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  LLM Provider reasoning_content Tests\n");
    printf("=========================================\n");

    test_build_request_echoes_reasoning();
    test_parse_response_reads_reasoning();
    test_parse_response_without_reasoning();
    test_buf_append();

    printf("\nAll provider reasoning tests PASSED\n");
    return 0;
}
