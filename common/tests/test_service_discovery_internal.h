// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_service_discovery_internal.h
 * @brief 服务发现单元测试拆分后的跨文件共享声明（测试宏、共享辅助与各域测试函数）
 *
 * @details
 * test_service_discovery.c 拆分为主文件 + 多个功能域文件后，
 * 共享的测试宏（TEST/PASS/FAIL/ASSERT）、运行计数器、实例工厂
 * make_instance()、事件回调 event_callback() 以及全部测试函数
 * 的声明统一放置于此，供各域测试文件包含。
 */

#ifndef AIRY_RT_TEST_SERVICE_DISCOVERY_INTERNAL_H
#define AIRY_RT_TEST_SERVICE_DISCOVERY_INTERNAL_H

#include "../include/service_discovery.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int tests_run;
extern int tests_passed;

#define TEST(name)               \
    do {                         \
        tests_run++;             \
        printf("  %-50s", name); \
    } while (0)
#define PASS()              \
    do {                    \
        tests_passed++;     \
        printf("[PASS]\n"); \
    } while (0)
#define FAIL(msg)                   \
    do {                            \
        printf("[FAIL] %s\n", msg); \
        return;                     \
    } while (0)
#define ASSERT(cond, msg) \
    do {                  \
        if (!(cond)) {    \
            FAIL(msg);    \
        }                 \
    } while (0)

extern int callback_fired;
extern sd_event_type_t last_event;

/* 共享辅助（定义于主文件 test_service_discovery.c） */
sd_instance_t make_instance(const char *id, const char *endpoint);

void event_callback(sd_event_type_t event, const char *svc, const sd_instance_t *inst,
                    void *user_data);

/* ==== lifecycle 域 ==== */
void test_sd_create_default_config(void);
void test_sd_create_null_config(void);
void test_sd_create_with_config(void);
void test_sd_destroy_null(void);
void test_sd_lifecycle(void);
void test_sd_start_idempotent(void);
void test_sd_stop_null(void);
void test_sd_register_normal(void);
void test_sd_register_null_params(void);
void test_sd_deregister_normal(void);
void test_sd_deregister_nonexistent(void);
void test_sd_deregister_all(void);

/* ==== discover 域 ==== */
void test_sd_discover_normal(void);
void test_sd_discover_nonexistent(void);
void test_sd_discover_by_type(void);
void test_sd_discover_by_tags(void);
void test_sd_get_dependencies(void);
void test_sd_check_dependencies(void);
void test_sd_discover_max_count(void);
void test_sd_discover_only_healthy(void);

/* ==== select 域 ==== */
void test_sd_select_instance_round_robin(void);
void test_sd_select_instance_random(void);
void test_sd_select_instance_nonexistent(void);
void test_sd_select_instance_weighted(void);
void test_sd_select_instance_least_connection(void);
void test_sd_select_instance_least_load(void);

/* ==== health 域 ==== */
void test_sd_heartbeat_normal(void);
void test_sd_heartbeat_nonexistent(void);
void test_sd_update_health(void);
void test_sd_update_connections(void);

/* ==== misc 域 ==== */
void test_sd_register_event_callback(void);
void test_sd_get_stats(void);
void test_sd_service_count(void);
void test_sd_lb_strategy_to_string(void);
void test_sd_multiple_services_stress(void);
void test_sd_null_parameter_safety(void);
void test_sd_callback_health_changes(void);
void test_sd_deregister_all_nonexistent(void);
void test_sd_multiple_callbacks(void);
void test_sd_discover_null_params(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TEST_SERVICE_DISCOVERY_INTERNAL_H */
