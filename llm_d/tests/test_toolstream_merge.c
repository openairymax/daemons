// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_toolstream_merge.c
 * @brief 流式 tool_calls 装配回归（缺 index 的上游不得把一次调用切成多槽）
 *
 * OpenAI 流式协议以 tool_calls[].index 为分片归并键，但 llama.cpp 服务端与
 * 部分中转网关整条流不下发 index：首帧给 id+name，后续帧只给 arguments 分片。
 * 旧实现在缺 index 时以「当前槽数」作新键，同一次调用被切成 N 个槽——name
 * 只落首片、参数落进空名槽，流末装配出缺 function.name 的 tool_call，执行侧
 * 必然「Tool not found」。本测试锁定三条不变量：
 *
 *   1. 缺 index 的参数分片续写同一槽（按 id 归并 / 无 id 续最后一个槽）
 *   2. 显式 index 的交错分片不得串槽
 *   3. 无名槽在装配期被丢弃，绝不输出缺 name 的 tool_call
 */

#include "airy_prov/toolstream.h"

#include <cjson_helpers.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

static void delta(provider_tool_acc_t *acc, const char *json)
{
    cJSON *d = cJSON_Parse(json);
    if (!d) {
        printf("    FAIL: delta json parse: %s\n", json);
        g_fail++;
        return;
    }
    provider_tool_delta(acc, d);
    cJSON_Delete(d);
}

static void check(const char *name, int cond)
{
    if (!cond) {
        printf("    FAIL: %s\n", name);
        g_fail++;
    }
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; p && (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

/* 1. llama.cpp 兼容端：首帧 id+name，后续帧只有 arguments，全程无 index。 */
static void test_indexless_fragments_merge(void)
{
    printf("  test_indexless_fragments_merge...\n");
    provider_tool_acc_t acc;
    memset(&acc, 0, sizeof(acc));

    delta(&acc, "{\"tool_calls\":[{\"id\":\"call_a\",\"type\":\"function\","
                "\"function\":{\"name\":\"fs_read\",\"arguments\":\"{\\\"path\\\":\\\"\"}}]}");
    delta(&acc, "{\"tool_calls\":[{\"function\":{\"arguments\":\"\\\"/tmp/x\\\"}\"}}]}");

    check("one slot", acc.count == 1);
    char *js = provider_tool_json(&acc);
    check("json produced", js != NULL);
    check("name kept", js && strstr(js, "fs_read") != NULL);
    check("id kept", js && strstr(js, "call_a") != NULL);
    check("args merged", js && strstr(js, "/tmp/x") != NULL);
    check("single entry", js && count_of(js, "\"type\":\"function\"") == 1);
    free(js);
    provider_tool_free(&acc);
}

/* 2. 显式 index：两个调用交错分片，各归各槽，参数不得串味。 */
static void test_explicit_index_interleaved(void)
{
    printf("  test_explicit_index_interleaved...\n");
    provider_tool_acc_t acc;
    memset(&acc, 0, sizeof(acc));

    delta(&acc, "{\"tool_calls\":[{\"index\":0,\"id\":\"c0\",\"function\":"
                "{\"name\":\"alpha\",\"arguments\":\"{\\\"n\\\":\"}}]}");
    delta(&acc, "{\"tool_calls\":[{\"index\":1,\"id\":\"c1\",\"function\":"
                "{\"name\":\"beta\",\"arguments\":\"{\\\"m\\\":\"}}]}");
    delta(&acc, "{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"1}\"}}]}");
    delta(&acc, "{\"tool_calls\":[{\"index\":1,\"function\":{\"arguments\":\"2}\"}}]}");

    check("two slots", acc.count == 2);
    char *js = provider_tool_json(&acc);
    check("alpha args", js && strstr(js, "{\\\"n\\\":1}") != NULL);
    check("beta args", js && strstr(js, "{\\\"m\\\":2}") != NULL);
    free(js);
    provider_tool_free(&acc);
}

/* 3. 无名槽（无主参数碎片）在装配期丢弃，不产出缺 name 的 tool_call。 */
static void test_nameless_slot_dropped(void)
{
    printf("  test_nameless_slot_dropped...\n");
    provider_tool_acc_t acc;
    memset(&acc, 0, sizeof(acc));

    delta(&acc, "{\"tool_calls\":[{\"function\":{\"arguments\":\"{}\"}}]}");
    check("slot accumulated", acc.count == 1);
    char *js = provider_tool_json(&acc);
    check("empty array", js && strcmp(js, "[]") == 0);
    free(js);
    provider_tool_free(&acc);
}

/* 4. 缺 index 但 id 变化：新调用另立槽，不复用上一槽（保留并行调用能力）。 */
static void test_indexless_new_id(void)
{
    printf("  test_indexless_new_id...\n");
    provider_tool_acc_t acc;
    memset(&acc, 0, sizeof(acc));

    delta(&acc, "{\"tool_calls\":[{\"id\":\"c0\",\"function\":"
                "{\"name\":\"alpha\",\"arguments\":\"{}\"}}]}");
    delta(&acc, "{\"tool_calls\":[{\"id\":\"c1\",\"function\":"
                "{\"name\":\"beta\",\"arguments\":\"{}\"}}]}");

    check("two slots", acc.count == 2);
    char *js = provider_tool_json(&acc);
    check("both named", js && strstr(js, "alpha") != NULL && strstr(js, "beta") != NULL);
    free(js);
    provider_tool_free(&acc);
}

int main(void)
{
    printf("=========================================\n");
    printf("  LLM Provider tool_calls Stream Merge\n");
    printf("=========================================\n");

    test_indexless_fragments_merge();
    test_explicit_index_interleaved();
    test_nameless_slot_dropped();
    test_indexless_new_id();

    if (g_fail) {
        printf("\n%d assertion(s) FAILED\n", g_fail);
        return 1;
    }
    printf("\nAll toolstream merge tests PASSED\n");
    return 0;
}
