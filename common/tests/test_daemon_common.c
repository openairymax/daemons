// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_daemon_common.c
 * @brief daemons/common 模块深度单元测试（P1-C06）——主文件
 *
 * 覆盖范围（补充现有10个测试文件未覆盖的关键API）：
 * - 熔断器（Circuit Breaker）完整生命周期与状态转换
 * - 配置管理器（Config Manager）类型化读写与命名空间
 * - 方法分发器（Method Dispatcher）注册与路由
 * - 告警管理器（Alert Manager）规则引擎与通知
 * - 服务生命周期（Service Lifecycle）创建/启停/状态查询
 *
 * 主文件承载：SPDX 头、断言宏与全局计数（经 test_daemon_common_internal.h）、
 * 共享 mock handler 与 service stub、int main()。测试函数按功能域拆分：
 *   - test_daemon_common_cb.c：熔断器
 *   - test_daemon_common_cm.c：配置管理器
 *   - test_daemon_common_md.c：方法分发器
 *   - test_daemon_common_am.c：告警管理器
 *   - test_daemon_common_svc.c：服务生命周期
 */

#include "test_daemon_common_internal.h"

int g_tests_run = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;

int g_dispatch_called = 0;
int g_dispatch_id = -1;

void dummy_handler(cJSON *params, int id, void *user_data)
{
    (void)params;
    (void)user_data;
    g_dispatch_called++;
    g_dispatch_id = id;
}

int g_md_call_a = 0, g_md_call_b = 0;

void md_handler_a(cJSON *p, int id, void *ud)
{
    (void)p;
    (void)id;
    (void)ud;
    g_md_call_a++;
}
void md_handler_b(cJSON *p, int id, void *ud)
{
    (void)p;
    (void)id;
    (void)ud;
    g_md_call_b++;
}

int g_v1_calls = 0, g_v2_calls = 0;

void v1_handler(cJSON *p, int id, void *ud)
{
    (void)p;
    (void)id;
    (void)ud;
    g_v1_calls++;
}
void v2_handler(cJSON *p, int id, void *ud)
{
    (void)p;
    (void)id;
    (void)ud;
    g_v2_calls++;
}

/* ==================== Service lifecycle stubs ==================== */
airy_err_t svc_dummy_init(airy_svc_t svc, const airy_svc_config_t *cfg)
{
    (void)svc;
    (void)cfg;
    return AIRY_SUCCESS;
}

airy_err_t svc_dummy_start(airy_svc_t svc)
{
    (void)svc;
    return AIRY_SUCCESS;
}

airy_err_t svc_dummy_stop(airy_svc_t svc, bool force)
{
    (void)svc;
    (void)force;
    return AIRY_SUCCESS;
}

void svc_dummy_destroy(airy_svc_t svc)
{
    (void)svc;
}

airy_err_t svc_dummy_healthcheck(airy_svc_t svc)
{
    (void)svc;
    return AIRY_SUCCESS;
}

airy_svc_interface_t make_dummy_interface(void)
{
    airy_svc_interface_t iface;
    iface.init = svc_dummy_init;
    iface.start = svc_dummy_start;
    iface.stop = svc_dummy_stop;
    iface.destroy = svc_dummy_destroy;
    iface.healthcheck = svc_dummy_healthcheck;
    return iface;
}

int main(void)
{
    printf("========================================\n");
    printf("  AgentRT daemons/common 深度单元测试套件\n");
    printf("  P1-C06: 目标覆盖率 >50%%\n");
    printf("========================================\n");

    /* 1. Circuit Breaker (8 tests) */
    test_cb_manager_lifecycle();
    test_cb_create_and_state();
    test_cb_failure_trip();
    test_cb_success_recovery();
    test_cb_force_operations();
    test_cb_timeout_recording();
    test_cb_multiple_breakers();
    test_cb_default_configs();

    /* 2. Config Manager (6 tests) */
    test_cm_init_shutdown();
    test_cm_set_get_basic();
    test_cm_typed_accessors();
    test_cm_namespace_ops();
    test_cm_environment();
    test_cm_export_and_entry_count();

    /* 3. Method Dispatcher (5 tests) */
    test_md_create_destroy();
    test_md_register_and_dispatch();
    test_md_not_found();
    test_md_multiple_methods();
    test_md_overwrite_registration();

    /* 4. Alert Manager (5 tests) */
    test_am_lifecycle();
    test_am_fire_resolve();
    test_am_all_levels();
    test_am_rules();
    test_am_query_and_utils();

    /* 5. Service Lifecycle (7 tests) */
    test_svc_create_destroy();
    test_svc_full_lifecycle();
    test_svc_state_strings();
    test_svc_capability_checks();
    test_svc_registry_operations();
    test_svc_user_data_and_metadata();

    printf("\n========================================\n");
    printf("  P1-C06 测试结果汇总\n");
    printf("========================================\n");
    printf("  总计:   %d\n", g_tests_run);
    printf("  通过:   %d\n", g_tests_passed);
    printf("  失败:   %d\n", g_tests_failed);
    printf("  通过率: %.1f%%\n",
           g_tests_run > 0 ? (double)g_tests_passed / g_tests_run * 100.0 : 0.0);
    printf("========================================\n");

    return g_tests_failed > 0 ? 1 : 0;
}
