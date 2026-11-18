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
#include "run/agent_verify.h"

#include "run/agent_run_engine.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
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

/* 判据静态门禁：只有「能交给 verifier 复核」的形态才算合法。 */
static void test_criteria_gate(void)
{
    CHECK(!agent_task_criteria_ok(NULL), "NULL criteria must be rejected");
    CHECK(!agent_task_criteria_ok(""), "empty criteria must be rejected");
    CHECK(!agent_task_criteria_ok("[]"), "empty array must be rejected");
    CHECK(!agent_task_criteria_ok("{\"criteria\":[]}"), "empty wrapped array must be rejected");
    CHECK(!agent_task_criteria_ok("\"done\""), "bare string must be rejected");

    /* 四类合法判据各一条。 */
    CHECK(agent_task_criteria_ok("[{\"type\":\"file\",\"path\":\"out/a.txt\"}]"),
          "file criteria must pass");
    CHECK(agent_task_criteria_ok("[{\"type\":\"cmd\",\"cmd\":\"true\"}]"), "cmd criteria must pass");
    CHECK(agent_task_criteria_ok("[{\"type\":\"service\",\"port\":2027}]"),
          "service criteria must pass");
    CHECK(agent_task_criteria_ok(
              "[{\"type\":\"rpc\",\"daemon\":\"tool\",\"method\":\"list_tools\"}]"),
          "rpc criteria must pass");
    CHECK(agent_task_criteria_ok("{\"criteria\":[{\"type\":\"cmd\",\"cmd\":\"true\"}]}"),
          "wrapped object must be accepted");

    /* 必需字段缺失 / 类型未知 / 端口越界：一律 fail-closed。 */
    CHECK(!agent_task_criteria_ok("[{\"type\":\"file\"}]"), "file without path must fail");
    CHECK(!agent_task_criteria_ok("[{\"type\":\"cmd\"}]"), "cmd without cmd must fail");
    CHECK(!agent_task_criteria_ok("[{\"type\":\"service\",\"port\":0}]"),
          "service port 0 must fail");
    CHECK(!agent_task_criteria_ok("[{\"type\":\"service\",\"port\":70000}]"),
          "service port 70000 must fail");
    CHECK(!agent_task_criteria_ok("[{\"type\":\"rpc\",\"daemon\":\"tool\"}]"),
          "rpc without method must fail");
    CHECK(!agent_task_criteria_ok("[{\"type\":\"shell\",\"cmd\":\"true\"}]"),
          "unknown type must fail");
    CHECK(!agent_task_criteria_ok("[{\"cmd\":\"true\"}]"), "missing type must fail");
}

static void test_criteria_count(void)
{
    /* 超过 verifier 上限（16）即非法：禁止把判据当脚本宿主使用。 */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < AGENT_TASK_CRITERIA_MAX + 1; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "type", "cmd");
        cJSON_AddStringToObject(it, "cmd", "true");
        cJSON_AddItemToArray(arr, it);
    }
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    CHECK(s != NULL, "serialize 17 criteria");
    if (s) {
        CHECK(!agent_task_criteria_ok(s), "17 criteria must exceed the cap");
        free(s);
    }

    /* 恰好 16 条仍合法，边界不提前收紧。 */
    arr = cJSON_CreateArray();
    for (int i = 0; i < AGENT_TASK_CRITERIA_MAX; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "type", "cmd");
        cJSON_AddStringToObject(it, "cmd", "true");
        cJSON_AddItemToArray(arr, it);
    }
    s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    CHECK(s != NULL, "serialize 16 criteria");
    if (s) {
        CHECK(agent_task_criteria_ok(s), "16 criteria must stay within the cap");
        free(s);
    }
}

static void test_criteria_from_plan(void)
{
    /* 计划派生只采纳显式对象判据；自然语言 postcondition 一律忽略，
     * 否则会把说明文字猜成命令，制造假失败。 */
    cJSON *plan = cJSON_CreateObject();
    cJSON *steps = cJSON_AddArrayToObject(plan, "steps");
    cJSON *s0 = cJSON_CreateObject();
    cJSON *post0 = cJSON_AddObjectToObject(s0, "postcondition");
    cJSON_AddStringToObject(post0, "type", "file");
    cJSON_AddStringToObject(post0, "path", "out/result.txt");
    cJSON_AddItemToArray(steps, s0);
    cJSON *s1 = cJSON_CreateObject();
    cJSON_AddStringToObject(s1, "postcondition", "确认服务已经启动");
    cJSON_AddItemToArray(steps, s1);
    cJSON *s2 = cJSON_CreateObject();
    cJSON *post2 = cJSON_AddArrayToObject(s2, "postcondition");
    cJSON *i0 = cJSON_CreateObject();
    cJSON_AddStringToObject(i0, "type", "cmd");
    cJSON_AddStringToObject(i0, "cmd", "test -f out/result.txt");
    cJSON_AddItemToArray(post2, i0);
    cJSON *i1 = cJSON_CreateObject();
    cJSON_AddStringToObject(i1, "type", "file");
    cJSON_AddStringToObject(i1, "path", "out/log.txt");
    cJSON_AddItemToArray(post2, i1);
    cJSON_AddItemToArray(steps, s2);

    char *derived = agent_task_criteria_from_plan(plan);
    CHECK(derived != NULL, "plan with typed postconditions must derive criteria");
    if (derived) {
        cJSON *arr = cJSON_Parse(derived);
        CHECK(cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 3,
              "derived criteria count must be 3");
        CHECK(agent_task_criteria_ok(derived), "derived criteria must be verifier-ready");
        cJSON_Delete(arr);
        free(derived);
    }
    cJSON_Delete(plan);

    /* 只有自然语言：派生不出任何判据，必须返回 NULL（上层记 unverified）。 */
    cJSON *plain = cJSON_CreateObject();
    cJSON *psteps = cJSON_AddArrayToObject(plain, "steps");
    cJSON *p0 = cJSON_CreateObject();
    cJSON_AddStringToObject(p0, "postcondition", "确认服务已经启动");
    cJSON_AddItemToArray(psteps, p0);
    CHECK(agent_task_criteria_from_plan(plain) == NULL,
          "natural language postcondition must not be guessed into criteria");
    cJSON_Delete(plain);

    CHECK(agent_task_criteria_from_plan(NULL) == NULL, "NULL plan must derive nothing");
}

static void test_needs_criteria(void)
{
    agent_run_task_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    snprintf(meta.task_id, sizeof(meta.task_id), "task-1-1");
    snprintf(meta.mode, sizeof(meta.mode), "task");
    snprintf(meta.mode_reason, sizeof(meta.mode_reason), "%s", AGENT_TASK_REASON_EXPLICIT_TASK);

    cJSON *out = agent_task_build_needs_criteria(&meta, NULL);
    CHECK(out != NULL, "build_needs_criteria");
    if (out) {
        cJSON *st = cJSON_GetObjectItem(out, "status");
        cJSON *tid = cJSON_GetObjectItem(out, "task_id");
        cJSON *ex = cJSON_GetObjectItem(out, "example");
        cJSON *arr = ex ? cJSON_GetObjectItem(ex, "criteria") : NULL;
        CHECK(cJSON_IsString(st) &&
                  strcmp(st->valuestring, AGENT_TASK_STATUS_NEEDS_CRITERIA) == 0,
              "needs_criteria status");
        CHECK(cJSON_IsString(tid) && strcmp(tid->valuestring, "task-1-1") == 0,
              "needs_criteria task_id");
        CHECK(cJSON_IsArray(arr) && cJSON_GetArraySize(arr) >= 1,
              "needs_criteria example.criteria must be a non-empty array");
        cJSON_Delete(out);
    }
}

/* 判据缺失/不可解析时不得猜测通过：归因为「无法取证」，且不产生结果对象。 */
static void test_verify_unverified(void)
{
    cJSON *v = (cJSON *)1;
    CHECK(agent_verify_apply(NULL, &v) == AGENT_RUN_RC_UNVERIFIED,
          "NULL criteria must be unverified");
    CHECK(v == NULL, "unverified must not fabricate verification payload");

    v = (cJSON *)1;
    CHECK(agent_verify_apply("[]", &v) == AGENT_RUN_RC_UNVERIFIED,
          "empty criteria must be unverified");
    CHECK(v == NULL, "unverified must leave verification NULL");

    CHECK(agent_verify_apply(NULL, NULL) == AGENT_RUN_RC_UNVERIFIED,
          "NULL out pointer must still be unverified");
}

int main(void)
{
    test_mode_rules();
    test_task_id();
    test_gccp_gate();
    test_gate_decision();
    test_criteria_gate();
    test_criteria_count();
    test_criteria_from_plan();
    test_needs_criteria();
    test_verify_unverified();
    if (g_failures) {
        printf("test_task_orchestrate: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_task_orchestrate: all checks passed\n");
    return 0;
}
