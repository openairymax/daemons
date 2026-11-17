// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_task_orchestrate.c
 * @brief 统一任务编排入口的裁决/身份单测（C-15）。
 *
 * 覆盖 §16.14.3 的规则基线：资源操作动词、工程动词、长目标、短对话、
 * 显式声明，以及 task_id 生成的格式与唯一性。
 */

#include "run/agent_task_contract.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static void expect_mode(const char *goal, const char *declared, const char *want_mode,
                        const char *want_reason)
{
    char mode[8] = {0};
    char reason[AGENT_TASK_REASON_LEN] = {0};
    int rc = agent_task_decide_mode(goal, declared, mode, sizeof(mode), reason, sizeof(reason));
    CHECK(rc == 0, "decide_mode(%s) rc=%d", goal, rc);
    CHECK(strcmp(mode, want_mode) == 0, "%s: mode=%s want=%s", goal, mode, want_mode);
    CHECK(strcmp(reason, want_reason) == 0, "%s: reason=%s want=%s", goal, reason, want_reason);
}

static void test_mode_rules(void)
{
    /* §16.14.3 实测基线 */
    expect_mode("打开火狐", NULL, AGENT_TASK_MODE_TASK, AGENT_TASK_REASON_RESOURCE_VERB);
    expect_mode("安装火狐", NULL, AGENT_TASK_MODE_TASK, AGENT_TASK_REASON_ENGINEERING_VERB);
    expect_mode("你好呀", NULL, AGENT_TASK_MODE_CHAT, AGENT_TASK_REASON_SHORT_CHAT);
    expect_mode("你好呀", "chat", AGENT_TASK_MODE_CHAT, AGENT_TASK_REASON_EXPLICIT_CHAT);
    expect_mode("打开火狐", "chat", AGENT_TASK_MODE_CHAT, AGENT_TASK_REASON_EXPLICIT_CHAT);
    expect_mode("你好呀", "task", AGENT_TASK_MODE_TASK, AGENT_TASK_REASON_EXPLICIT_TASK);

    /* 长目标：128 个码点优先于动词规则 */
    char long_goal[512] = {0};
    for (int i = 0; i < 128; i++)
        strcat(long_goal, "字");
    expect_mode(long_goal, NULL, AGENT_TASK_MODE_TASK, AGENT_TASK_REASON_LONG_GOAL);

    /* 无任务证据的普通句子保持对话 */
    expect_mode("今天天气怎么样", NULL, AGENT_TASK_MODE_CHAT, AGENT_TASK_REASON_DEFAULT_CHAT);

    /* auto 与 NULL 等价 */
    expect_mode("打开火狐", "auto", AGENT_TASK_MODE_TASK, AGENT_TASK_REASON_RESOURCE_VERB);
}

static void test_task_id(void)
{
    char a[AGENT_TASK_ID_LEN] = {0};
    char b[AGENT_TASK_ID_LEN] = {0};
    agent_task_gen_id("task", a, sizeof(a));
    agent_task_gen_id("task", b, sizeof(b));
    CHECK(strncmp(a, "task-", 5) == 0, "task_id prefix: %s", a);
    CHECK(strlen(a) > 5 && strlen(a) < AGENT_TASK_ID_LEN, "task_id length: %s", a);
    CHECK(strcmp(a, b) != 0, "task_id must be unique: %s", a);

    char p[AGENT_TASK_PLAN_ID_LEN] = {0};
    agent_task_gen_id("plan", p, sizeof(p));
    CHECK(strncmp(p, "plan-", 5) == 0, "plan_id prefix: %s", p);
}

static void test_gccp_gate(void)
{
    agent_task_missing_slot_t missing[AGENT_TASK_MISSING_MAX];
    size_t count = 99;
    bool confirmed = false;

    /* 无答案：保持声明值 false，missing 为空（`打开火狐` 基线）。 */
    CHECK(agent_task_gccp_check(NULL, &confirmed, missing, AGENT_TASK_MISSING_MAX, &count) == 0,
          "gccp_check(NULL)");
    CHECK(!confirmed, "no answers must stay unconfirmed");
    CHECK(count == 0, "no answers must not report missing slots");

    /* 空对象：四个槽全缺。 */
    cJSON *empty = cJSON_CreateObject();
    confirmed = false;
    CHECK(agent_task_gccp_check(empty, &confirmed, missing, AGENT_TASK_MISSING_MAX, &count) == 0,
          "gccp_check({})");
    CHECK(!confirmed, "empty answers must stay unconfirmed");
    CHECK(count == 4, "empty answers missing=%zu want=4", count);
    cJSON_Delete(empty);

    /* 齐备答案：确认态成立。 */
    cJSON *full = cJSON_CreateObject();
    cJSON_AddStringToObject(full, "goal", "打开火狐");
    cJSON_AddStringToObject(full, "context", "桌面会话");
    cJSON_AddStringToObject(full, "constraints", "不改变书签");
    cJSON_AddStringToObject(full, "plan", "启动并校验进程");
    confirmed = false;
    CHECK(agent_task_gccp_check(full, &confirmed, missing, AGENT_TASK_MISSING_MAX, &count) == 0,
          "gccp_check(full)");
    CHECK(confirmed && count == 0, "full answers must confirm (count=%zu)", count);

    /* needs_intent 结果形状：status/mode/mode_reason/gccp_confirmed/gccp_missing。 */
    cJSON *out = agent_task_build_needs_intent(NULL, missing, 0);
    CHECK(out != NULL, "build_needs_intent");
    if (out) {
        cJSON *st = cJSON_GetObjectItem(out, "status");
        cJSON *cf = cJSON_GetObjectItem(out, "gccp_confirmed");
        CHECK(cJSON_IsString(st) && strcmp(st->valuestring, "needs_intent") == 0,
              "needs_intent status");
        CHECK(cJSON_IsBool(cf) && !cJSON_IsTrue(cf), "needs_intent gccp_confirmed=false");
        CHECK(cJSON_IsArray(cJSON_GetObjectItem(out, "gccp_missing")), "gccp_missing array");
        CHECK(cJSON_IsArray(cJSON_GetObjectItem(out, "missing")), "missing alias array");
        cJSON_Delete(out);
    }
    cJSON_Delete(full);
}

static void test_gate_decision(void)
{
    /* gate:lab：`-p` 未确认（gccp_confirmed=false）时 task 门禁关闭，
     * 调用方必须在此之后、引擎之前返回 needs_intent——因此不可能产生
     * 任何 tool_start/tool_end 事件。chat 与已确认任务一律放行。 */
    CHECK(!agent_task_gate_allows(AGENT_TASK_MODE_TASK, false),
          "unconfirmed task must be gated");
    CHECK(agent_task_gate_allows(AGENT_TASK_MODE_TASK, true), "confirmed task must pass");
    CHECK(agent_task_gate_allows(AGENT_TASK_MODE_CHAT, false), "chat must never be gated");
    CHECK(agent_task_gate_allows(NULL, false), "unknown mode defaults to pass");
}

int main(void)
{
    test_mode_rules();
    test_task_id();
    test_gccp_gate();
    test_gate_decision();
    if (g_failures) {
        printf("test_task_orchestrate: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_task_orchestrate: all checks passed\n");
    return 0;
}
