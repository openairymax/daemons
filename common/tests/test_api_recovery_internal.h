// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_api_recovery_internal.h
 *   API Recovery 模块单元测试拆分后的共享内部头
 *
 * 共享内容：断言宏与全局计数。
 * 按功能域拆分：
 *   - test_api_recovery_pool.c：池生命周期（4 用例）
 *   - test_api_recovery_cred.c：凭据增删/轮转/标记（15 用例）
 *   - test_api_recovery_health.c：健康度查询/衰减/恢复/禁用（7 用例）
 *   - test_api_recovery_fallback.c：降级模型/降级升级链路（12 用例）
 *   - test_api_recovery_config.c：重试配置/熔断绑定/统计（8 用例）
 *   - test_api_recovery_misc.c：字符串转换/全流程（3 用例）
 */

#ifndef TEST_API_RECOVERY_INTERNAL_H
#define TEST_API_RECOVERY_INTERNAL_H

#include "../include/api_recovery.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* 各域测试函数 */
void test_pool_create_null_name(void);
void test_pool_create_with_name(void);
void test_pool_destroy_null_safe(void);
void test_pool_multiple_create_destroy(void);

void test_add_credential(void);
void test_add_credential_null_pool(void);
void test_add_credential_null_key(void);
void test_add_credential_overflow(void);
void test_remove_credential(void);
void test_remove_credential_null_pool(void);
void test_remove_credential_out_of_range(void);
void test_next_credential_rotation(void);
void test_next_credential_null_pool(void);
void test_next_credential_empty_pool(void);
void test_mark_cred_success(void);
void test_mark_cred_failure(void);
void test_mark_cred_success_null_pool(void);
void test_mark_cred_failure_null_pool(void);
void test_mark_cred_empty_pool(void);

void test_cred_health(void);
void test_cred_health_null_pool(void);
void test_cred_health_out_of_range(void);
void test_credential_health_decay_after_failures(void);
void test_credential_health_recovery_on_success(void);
void test_credential_auth_failure_disables(void);
void test_remove_credential_adjusts_index(void);

void test_add_fallback_model(void);
void test_add_fallback_model_null_pool(void);
void test_add_fallback_model_null_name(void);
void test_add_fallback_model_overflow(void);
void test_current_model_primary(void);
void test_current_model_null_pool(void);
void test_degrade_upgrade_cycle(void);
void test_degrade_null_pool(void);
void test_upgrade_null_pool(void);
void test_degrade_without_fallbacks(void);
void test_upgrade_at_top(void);
void test_current_level_null_pool(void);

void test_set_retry_config(void);
void test_set_retry_config_null_pool(void);
void test_set_retry_config_edge_values(void);
void test_bind_circuit_breaker(void);
void test_bind_circuit_breaker_null_pool(void);
void test_get_stats(void);
void test_get_stats_null_pool(void);
void test_get_stats_null_output(void);

void test_error_string(void);
void test_degradation_string(void);
void test_full_workflow(void);

#endif /* TEST_API_RECOVERY_INTERNAL_H */
