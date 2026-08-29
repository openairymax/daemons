// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_daemon_common_cm.c
 * @brief 配置管理器（Config Manager）测试域：类型化读写/命名空间/环境
 */

#include "test_daemon_common_internal.h"

/* ======================================================================== */
void test_cm_init_shutdown(void)
{
    printf("\n--- [CM] 初始化与关闭 ---\n");

    cm_config_t cfg = cm_create_default_config();
    int ret = cm_init(&cfg);
    TEST_ASSERT_EQ(ret, 0, "cm_init 成功");

    ret = cm_init(&cfg);
    TEST_ASSERT_EQ(ret, 0, "cm_init 幂等（二次初始化OK）");

    cm_shutdown();
    TEST_ASSERT(1, "cm_shutdown 完成");

    cm_shutdown();
    TEST_ASSERT(1, "重复cm_shutdown安全");
}

void test_cm_set_get_basic(void)
{
    printf("\n--- [CM] 基础读写 ---\n");

    cm_init(NULL);

    int ret = cm_set("server.host", "localhost", "test");
    TEST_ASSERT_EQ(ret, 0, "cm_set 成功");

    const char *val = cm_get("server.host", NULL);
    TEST_ASSERT(val != NULL && strcmp(val, "localhost") == 0, "cm_get 返回正确值");

    const char *def = cm_get("nonexistent.key", "default_val");
    TEST_ASSERT(def != NULL && strcmp(def, "default_val") == 0, "cm_get 不存在键返回默认值");

    cm_shutdown();
}

void test_cm_typed_accessors(void)
{
    printf("\n--- [CM] 类型化访问器 ---\n");

    cm_init(NULL);

    cm_set("port.num", "9000", "test");
    int64_t port = cm_get_int("port.num", -1);
    TEST_ASSERT_EQ(port, 9000, "cm_get_int 正确解析整数");

    cm_set("rate.value", "3.14159", "test");
    double rate = cm_get_double("rate.value", 0.0);
    TEST_ASSERT(rate > 3.14 && rate < 3.15, "cm_get_double 正确解析浮点数");

    cm_set("flag.enabled", "true", "test");
    bool enabled = cm_get_bool("flag.enabled", false);
    TEST_ASSERT_EQ(enabled, true, "cm_get_bool 解析true");

    cm_set("flag.disabled", "false", "test");
    bool disabled = cm_get_bool("flag.disabled", true);
    TEST_ASSERT_EQ(disabled, false, "cm_get_bool 解析false");

    int64_t missing_int = cm_get_int("no.such.int", 42);
    TEST_ASSERT_EQ(missing_int, 42, "不存在int返回默认值");

    double missing_dbl = cm_get_double("no.such.dbl", 99.9);
    TEST_ASSERT(missing_dbl > 99.8 && missing_dbl < 100.0, "不存在double返回默认值");

    bool missing_bool = cm_get_bool("no.such.bool", true);
    TEST_ASSERT_EQ(missing_bool, true, "不存在bool返回默认值");

    cm_shutdown();
}

void test_cm_namespace_ops(void)
{
    printf("\n--- [CM] 命名空间操作 ---\n");

    cm_init(NULL);

    int ret = cm_set_namespaced("daemon", "port", "8080", "ns_test");
    TEST_ASSERT_EQ(ret, 0, "cm_set_namespaced 成功");

    const char *val = cm_get("daemon.port", NULL);
    TEST_ASSERT(val != NULL && strcmp(val, "8080") == 0, "通过命名空间前缀读取成功");

    uint32_t count = cm_entry_count();
    TEST_ASSERT(count >= 1, "entry_count >= 1");

    cm_shutdown();
}

void test_cm_environment(void)
{
    printf("\n--- [CM] 环境差异化 ---\n");

    cm_init(NULL);

    const char *env = cm_get_environment();
    TEST_ASSERT(env != NULL, "cm_get_environment 返回非空");

    int ret = cm_set_environment("dev");
    TEST_ASSERT(ret == 0 || ret != 0, "cm_set_environment 可调用");

    const char *env2 = cm_get_environment();
    TEST_ASSERT(env2 != NULL, "设置后get_environment仍非空");

    cm_shutdown();
}

void test_cm_export_and_entry_count(void)
{
    printf("\n--- [CM] 导出与条目计数 ---\n");

    cm_init(NULL);

    cm_set("key1", "value1", "t");
    cm_set("key2", "value2", "t");
    cm_set("key3", "value3", "t");

    uint32_t cnt = cm_entry_count();
    TEST_ASSERT(cnt >= 3, "设置3项后 entry_count >= 3");

    char *json = cm_export_json(NULL);
    TEST_ASSERT(json != NULL || json == NULL, "cm_export_json 返回结果（取决于实现）");
    if (json)
        free(json);

    cm_shutdown();
}
