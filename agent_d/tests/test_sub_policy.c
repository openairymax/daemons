// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_sub_policy.c
 * @brief 子任务委派策略面直测（white-box include，无 daemon 依赖）：
 *        三层配额优先级（配置 > 环境变量 > 缺省）、上限夹紧、线程内深度
 *        进出、并发槽位占用与归还、工具名全等判定、参数契约 JSON 形状。
 */

#include "../src/run/agent_sub_policy.c"

#include <stdio.h>
#include <string.h>

#include <cjson/cJSON.h>

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

static void env_clear(void)
{
    unsetenv(AGENT_SUB_ENV_DEPTH);
    unsetenv(AGENT_SUB_ENV_CONC);
    sub_limits_set(0, 0);
}

static void test_defaults(void)
{
    env_clear();
    agent_sub_limits_t lim = sub_limits();
    CHECK(lim.max_depth == AGENT_SUB_DEPTH_DEFAULT, "default depth");
    CHECK(lim.max_conc == AGENT_SUB_CONC_DEFAULT, "default conc");
}

static void test_env(void)
{
    env_clear();
    setenv(AGENT_SUB_ENV_DEPTH, "5", 1);
    setenv(AGENT_SUB_ENV_CONC, "9", 1);
    agent_sub_limits_t lim = sub_limits();
    CHECK(lim.max_depth == 5, "env depth");
    CHECK(lim.max_conc == 9, "env conc");

    setenv(AGENT_SUB_ENV_DEPTH, "999", 1);
    setenv(AGENT_SUB_ENV_CONC, "999", 1);
    lim = sub_limits();
    CHECK(lim.max_depth == AGENT_SUB_DEPTH_CAP, "env depth clamped");
    CHECK(lim.max_conc == AGENT_SUB_CONC_CAP, "env conc clamped");

    setenv(AGENT_SUB_ENV_DEPTH, "0", 1);
    setenv(AGENT_SUB_ENV_CONC, "-3", 1);
    lim = sub_limits();
    CHECK(lim.max_depth == AGENT_SUB_DEPTH_DEFAULT, "env zero -> default");
    CHECK(lim.max_conc == AGENT_SUB_CONC_DEFAULT, "env negative -> default");

    setenv(AGENT_SUB_ENV_DEPTH, "x7", 1);
    lim = sub_limits();
    CHECK(lim.max_depth == AGENT_SUB_DEPTH_DEFAULT, "env junk -> default");
}

static void test_config_wins(void)
{
    env_clear();
    setenv(AGENT_SUB_ENV_DEPTH, "7", 1);
    sub_limits_set(3, 2);
    agent_sub_limits_t lim = sub_limits();
    CHECK(lim.max_depth == 3, "config depth > env");
    CHECK(lim.max_conc == 2, "config conc > env");

    sub_limits_set(999, 999);
    lim = sub_limits();
    CHECK(lim.max_depth == AGENT_SUB_DEPTH_CAP, "config depth clamped");
    CHECK(lim.max_conc == AGENT_SUB_CONC_CAP, "config conc clamped");
    env_clear();
}

static void test_depth(void)
{
    env_clear();
    sub_limits_set(2, 4);
    CHECK(sub_depth() == 0, "depth starts 0");
    CHECK(sub_enter() == 0, "enter level 1");
    CHECK(sub_enter() == 0, "enter level 2");
    CHECK(sub_depth() == 2, "depth 2");
    CHECK(sub_enter() == 1, "enter over cap rejected");
    sub_leave();
    sub_leave();
    CHECK(sub_depth() == 0, "leave back to 0");
    sub_leave();
    CHECK(sub_depth() == 0, "leave floor 0");
}

static void test_slots(void)
{
    env_clear();
    sub_limits_set(2, 2);
    CHECK(sub_slot_try() == 1, "slot 1");
    CHECK(sub_slot_try() == 1, "slot 2");
    CHECK(sub_slot_try() == 0, "slot full rejected");
    sub_slot_drop();
    CHECK(sub_slot_try() == 1, "slot reused after drop");
    sub_slot_drop();
    sub_slot_drop();
}

static void test_tool_name(void)
{
    CHECK(sub_is_tool(AGENT_SUB_TOOL) == 1, "exact name matched");
    CHECK(sub_is_tool("subagent") == 0, "prefix not matched");
    CHECK(sub_is_tool("subagent.spawn.x") == 0, "suffix not matched");
    CHECK(sub_is_tool(NULL) == 0, "NULL rejected");
}

static void test_contract(void)
{
    cJSON *schema = cJSON_Parse(sub_schema());
    CHECK(schema != NULL, "schema parses");
    if (schema) {
        cJSON *props = cJSON_GetObjectItem(schema, "properties");
        CHECK(cJSON_GetObjectItem(props, "goal") != NULL, "schema goal");
        CHECK(cJSON_GetObjectItem(props, "context") != NULL, "schema context");
        CHECK(cJSON_GetObjectItem(props, "budget") != NULL, "schema budget");
        cJSON_Delete(schema);
    }
    CHECK(sub_desc() != NULL && sub_desc()[0], "desc non-empty");
    CHECK(sub_lead() != NULL && sub_lead()[0], "lead non-empty");
}

int main(void)
{
    test_defaults();
    test_env();
    test_config_wins();
    test_depth();
    test_slots();
    test_tool_name();
    test_contract();
    if (g_fail) {
        printf("FAILED %d case(s)\n", g_fail);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
