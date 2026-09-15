// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_run_loop.c
 * @brief 工具循环纯函数直测（white-box include，无 daemon 依赖）：
 *        C-1 连败计数语义 + LLM 响应解析兼容。
 */

#include "../src/agent_run_loop.c"

#include <stdio.h>

/* engine.c 引用的 daemon 全局服务句柄：产品侧由 main.c 定义，测试环境
 * 无 daemon 生命周期，NULL 即未就绪语义（与 main.c 初始值一致）。 */
#include "agent_d_internal.h"
agent_service_t *g_service = NULL;

static int g_fail = 0;

#define CHECK(cond, name)                \
    do {                                 \
        if (cond) {                      \
            printf("PASS %s\n", (name)); \
        } else {                         \
            printf("FAIL %s\n", (name)); \
            g_fail++;                    \
        }                                \
    } while (0)

static void test_fail_streak(void)
{
    int s = 0;
    CHECK(!run_fail_streak_bump(&s, 0) && s == 0, "bump: success resets to zero");
    CHECK(!run_fail_streak_bump(&s, -1) && s == 1, "bump: first failure counts");
    CHECK(!run_fail_streak_bump(&s, -1) && s == 2, "bump: streak accumulates");
    CHECK(run_fail_streak_bump(&s, -1) && s == 3, "bump: fuse trips at K");
    CHECK(run_fail_streak_bump(&s, -1) && s == 4, "bump: stays tripped");
    CHECK(!run_fail_streak_bump(&s, 0) && s == 0, "bump: success clears tripped state");
    CHECK(AGENT_RUN_TOOL_FAIL_LIMIT == 3, "bump: K constant is 3");
}

static void test_parse_tool_calls(void)
{
    cJSON *tc = NULL;
    const char *resp = "{\"choices\":[{\"content\":\"\",\"tool_calls\":[{\"id\":\"c1\","
                       "\"function\":{\"name\":\"fs_read\",\"arguments\":\"{}\"}}]}]}";
    CHECK(run_parse_tool_calls(resp, &tc) == 0 && tc && cJSON_GetArraySize(tc) == 1,
          "parse_tc: standard shape");
    cJSON_Delete(tc);

    tc = NULL;
    resp = "{\"result\":{\"choices\":[{\"tool_calls\":[{\"id\":\"c2\",\"function\":"
           "{\"name\":\"t\",\"arguments\":\"{}\"}}]}]}}";
    CHECK(run_parse_tool_calls(resp, &tc) == 0 && tc, "parse_tc: envelope unwrapped");
    cJSON_Delete(tc);

    tc = NULL;
    CHECK(run_parse_tool_calls("{\"choices\":[{\"content\":\"hi\"}]}", &tc) == -1 && tc == NULL,
          "parse_tc: none -> -1/NULL");
    CHECK(run_parse_tool_calls("not-json", &tc) == -1, "parse_tc: bad json -> -1");
}

static void test_parse_result(void)
{
    char *text = NULL;
    char *reasoning = NULL;
    uint64_t tokens = 0;
    double cost = 0.0;
    const char *resp = "{\"choices\":[{\"content\":\"ok\",\"reasoning_content\":\"think\"}],"
                       "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5},"
                       "\"cost_usd\":0.25}";
    CHECK(run_parse_result(resp, &text, &tokens, &cost, &reasoning) == 0, "parse: rc");
    CHECK(text && strcmp(text, "ok") == 0, "parse: content");
    CHECK(tokens == 15, "parse: usage sum");
    CHECK(cost == 0.25, "parse: cost");
    CHECK(reasoning && strcmp(reasoning, "think") == 0, "parse: reasoning");
    AIRY_FREE(text);
    AIRY_FREE(reasoning);

    text = NULL;
    tokens = 0;
    cost = 0.0;
    resp = "{\"result\":{\"choices\":[{\"content\":\"deep\"}],\"usage\":{\"total_tokens\":77}}}";
    CHECK(run_parse_result(resp, &text, &tokens, &cost, NULL) == 0, "parse2: rc");
    CHECK(text && strcmp(text, "deep") == 0 && tokens == 77, "parse2: envelope + total");
    AIRY_FREE(text);
}

int main(void)
{
    test_fail_streak();
    test_parse_tool_calls();
    test_parse_result();
    if (g_fail)
        printf("FAILURES: %d\n", g_fail);
    else
        printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
