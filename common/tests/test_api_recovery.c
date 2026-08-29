// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_api_recovery.c - API Recovery Module Unit Tests（主文件）
 *
 * 主文件承载：SPDX 头、断言宏与全局计数（经 test_api_recovery_internal.h）、
 * int main()。测试函数按功能域拆分（见 test_api_recovery_internal.h）。
 */

#include "test_api_recovery_internal.h"

int tests_run = 0;
int tests_passed = 0;

int main(void)
{
    printf("\n=== API Recovery Module Unit Tests ===\n\n");

    test_pool_create_null_name();
    test_pool_create_with_name();
    test_pool_destroy_null_safe();
    test_pool_multiple_create_destroy();
    test_add_credential();
    test_add_credential_null_pool();
    test_add_credential_null_key();
    test_add_credential_overflow();
    test_remove_credential();
    test_remove_credential_null_pool();
    test_remove_credential_out_of_range();
    test_next_credential_rotation();
    test_next_credential_null_pool();
    test_next_credential_empty_pool();
    test_mark_cred_success();
    test_mark_cred_failure();
    test_mark_cred_success_null_pool();
    test_mark_cred_failure_null_pool();
    test_mark_cred_empty_pool();
    test_cred_health();
    test_cred_health_null_pool();
    test_cred_health_out_of_range();
    test_add_fallback_model();
    test_add_fallback_model_null_pool();
    test_add_fallback_model_null_name();
    test_add_fallback_model_overflow();
    test_current_model_primary();
    test_current_model_null_pool();
    test_degrade_upgrade_cycle();
    test_degrade_null_pool();
    test_upgrade_null_pool();
    test_degrade_without_fallbacks();
    test_upgrade_at_top();
    test_current_level_null_pool();
    test_set_retry_config();
    test_set_retry_config_null_pool();
    test_set_retry_config_edge_values();
    test_bind_circuit_breaker();
    test_bind_circuit_breaker_null_pool();
    test_get_stats();
    test_get_stats_null_pool();
    test_get_stats_null_output();
    test_error_string();
    test_degradation_string();
    test_credential_health_decay_after_failures();
    test_credential_health_recovery_on_success();
    test_credential_auth_failure_disables();
    test_remove_credential_adjusts_index();
    test_full_workflow();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
