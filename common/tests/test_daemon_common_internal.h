// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_daemon_common_internal.h
 *   daemons/common 深度单元测试拆分后的共享内部头
 *
 * 共享内容：断言宏、全局计数、mock handler 与 service stub。
 * 按功能域拆分：
 *   - test_daemon_common_cb.c：熔断器（8 用例）
 *   - test_daemon_common_cm.c：配置管理器（6 用例）
 *   - test_daemon_common_md.c：方法分发器（5 用例）
 *   - test_daemon_common_am.c：告警管理器（5 用例）
 *   - test_daemon_common_svc.c：服务生命周期（7 用例）
 */

#ifndef TEST_DAEMON_COMMON_INTERNAL_H
#define TEST_DAEMON_COMMON_INTERNAL_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alert_manager.h"
#include "circuit_breaker.h"
#include "config_manager.h"
#include "error.h"
#include "method_dispatcher.h"
#include "svc_common.h"

extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;

#define TEST_ASSERT(cond, msg)                                \
    do {                                                      \
        g_tests_run++;                                        \
        if (cond) {                                           \
            g_tests_passed++;                                 \
            printf("  [PASS] %s\n", msg);                     \
        } else {                                              \
            g_tests_failed++;                                 \
            printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
        }                                                     \
    } while (0)

#define TEST_ASSERT_EQ(a, b, msg)                                                               \
    do {                                                                                        \
        g_tests_run++;                                                                          \
        if ((a) == (b)) {                                                                       \
            g_tests_passed++;                                                                   \
            printf("  [PASS] %s\n", msg);                                                       \
        } else {                                                                                \
            g_tests_failed++;                                                                   \
            printf("  [FAIL] %s: expected %ld, got %ld (line %d)\n", msg, (long)(b), (long)(a), \
                   __LINE__);                                                                   \
        }                                                                                       \
    } while (0)

#define TEST_ASSERT_NULL(ptr, msg) TEST_ASSERT((ptr) == NULL, msg)
#define TEST_ASSERT_NOT_NULL(ptr, msg) TEST_ASSERT((ptr) != NULL, msg)

/* mock handler 与 service stub（定义于 test_daemon_common.c 主文件） */
extern int g_dispatch_called;
extern int g_dispatch_id;

void dummy_handler(cJSON *params, int id, void *user_data);

extern int g_md_call_a;
extern int g_md_call_b;

void md_handler_a(cJSON *p, int id, void *ud);
void md_handler_b(cJSON *p, int id, void *ud);

extern int g_v1_calls;
extern int g_v2_calls;

void v1_handler(cJSON *p, int id, void *ud);
void v2_handler(cJSON *p, int id, void *ud);

airy_err_t svc_dummy_init(airy_svc_t svc, const airy_svc_config_t *cfg);
airy_err_t svc_dummy_start(airy_svc_t svc);
airy_err_t svc_dummy_stop(airy_svc_t svc, bool force);
void svc_dummy_destroy(airy_svc_t svc);
airy_err_t svc_dummy_healthcheck(airy_svc_t svc);
airy_svc_interface_t make_dummy_interface(void);

/* 各域测试函数 */
void test_cb_manager_lifecycle(void);
void test_cb_create_and_state(void);
void test_cb_failure_trip(void);
void test_cb_success_recovery(void);
void test_cb_force_operations(void);
void test_cb_timeout_recording(void);
void test_cb_multiple_breakers(void);
void test_cb_default_configs(void);

void test_cm_init_shutdown(void);
void test_cm_set_get_basic(void);
void test_cm_typed_accessors(void);
void test_cm_namespace_ops(void);
void test_cm_environment(void);
void test_cm_export_and_entry_count(void);

void test_md_create_destroy(void);
void test_md_register_and_dispatch(void);
void test_md_not_found(void);
void test_md_multiple_methods(void);
void test_md_overwrite_registration(void);

void test_am_lifecycle(void);
void test_am_fire_resolve(void);
void test_am_all_levels(void);
void test_am_rules(void);
void test_am_query_and_utils(void);

void test_svc_create_destroy(void);
void test_svc_full_lifecycle(void);
void test_svc_state_strings(void);
void test_svc_capability_checks(void);
void test_svc_registry_operations(void);
void test_svc_user_data_and_metadata(void);

#endif /* TEST_DAEMON_COMMON_INTERNAL_H */
