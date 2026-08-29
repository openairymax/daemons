// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_daemon_common_am.c
 * @brief 告警管理器（Alert Manager）测试域：触发/解决/级别/规则
 */

#include "test_daemon_common_internal.h"

/* ======================================================================== */
void test_am_lifecycle(void)
{
    printf("\n--- [AM] 初始化与关闭 ---\n");

    am_config_t cfg = am_create_default_config();
    int ret = am_init(&cfg);
    TEST_ASSERT_EQ(ret, 0, "am_init 成功");

    am_shutdown();
    TEST_ASSERT(1, "am_shutdown 完成");

    am_shutdown();
    TEST_ASSERT(1, "重复am_shutdown安全");
}

void test_am_fire_resolve(void)
{
    printf("\n--- [AM] 触发与解决告警 ---\n");

    am_init(NULL);

    int ret = am_fire("test_alert_01", AM_LEVEL_WARNING, "Test warning message", "unit_test", "");
    TEST_ASSERT_EQ(ret, 0, "am_fire WARNING 成功");

    uint32_t count = am_active_alert_count();
    TEST_ASSERT(count >= 1, "触发后活跃告警>=1");

    ret = am_resolve("test_alert_01");
    TEST_ASSERT_EQ(ret, 0, "am_resolve 成功");

    am_shutdown();
}

void test_am_all_levels(void)
{
    printf("\n--- [AM] 所有告警级别 ---\n");

    am_init(NULL);

    int r1 = am_fire("alert_info", AM_LEVEL_INFO, "Info msg", "src", "");
    TEST_ASSERT_EQ(r1, 0, "INFO级别触发成功");

    int r2 = am_fire("alert_warn", AM_LEVEL_WARNING, "Warn msg", "src", "");
    TEST_ASSERT_EQ(r2, 0, "WARNING级别触发成功");

    int r3 = am_fire("alert_crit", AM_LEVEL_CRITICAL, "Crit msg", "src", "");
    TEST_ASSERT_EQ(r3, 0, "CRITICAL级别触发成功");

    int r4 = am_fire("alert_emerg", AM_LEVEL_EMERGENCY, "Emerg msg", "src", "");
    TEST_ASSERT_EQ(r4, 0, "EMERGENCY级别触发成功");

    uint32_t total = am_active_alert_count();
    TEST_ASSERT(total >= 4, "4个不同级别告警全部活跃");

    am_shutdown();
}

void test_am_rules(void)
{
    printf("\n--- [AM] 告警规则 ---\n");

    am_init(NULL);

    am_rule_t rule = {0};
    strncpy(rule.name, "cpu_high_rule", AM_MAX_NAME_LEN - 1);
    rule.name[AM_MAX_NAME_LEN - 1] = '\0';
    rule.type = AM_RULE_THRESHOLD;
    rule.level = AM_LEVEL_WARNING;
    strncpy(rule.metric_name, "cpu_usage", sizeof(rule.metric_name) - 1);
    rule.metric_name[sizeof(rule.metric_name) - 1] = '\0';
    rule.comparison = AM_OP_GT;
    rule.threshold = 80.0;
    rule.duration_seconds = 5;
    rule.cooldown_seconds = 60;
    rule.enabled = true;

    int ret = am_add_rule(&rule);
    TEST_ASSERT_EQ(ret, 0, "am_add_rule 成功");

    ret = am_remove_rule("cpu_high_rule");
    TEST_ASSERT_EQ(ret, 0, "am_remove_rule 成功");

    am_shutdown();
}

void test_am_query_and_utils(void)
{
    printf("\n--- [AM] 查询与工具函数 ---\n");

    am_init(NULL);

    am_fire("query_test", AM_LEVEL_INFO, "Query test", "src", "");

    am_alert_t alerts[16];
    uint32_t found = 0;
    int ret = am_get_active_alerts(alerts, 16, &found);
    TEST_ASSERT_EQ(ret, 0, "am_get_active_alerts 成功");
    TEST_ASSERT(found >= 1, "找到>=1个活跃告警");

    uint32_t level_found = 0;
    ret = am_get_alerts_by_level(AM_LEVEL_INFO, alerts, 16, &level_found);
    TEST_ASSERT_EQ(ret, 0, "am_get_alerts_by_level 成功");

    const char *lvl_str = am_level_to_string(AM_LEVEL_CRITICAL);
    TEST_ASSERT(lvl_str != NULL && strlen(lvl_str) > 0, "level_to_string(CRITICAL) 有效");

    const char *st_str = am_state_to_string(AM_STATE_FIRING);
    TEST_ASSERT(st_str != NULL && strlen(st_str) > 0, "state_to_string(FIRING) 有效");

    am_acknowledge("query_test");
    TEST_ASSERT(1, "am_acknowledge 可调用");

    am_shutdown();
}
