// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_api_recovery_misc.c - 字符串转换/全流程测试域
 */

#include "test_api_recovery_internal.h"

void test_error_string(void)
{
    TEST("Error code to string conversion");
    ASSERT(strcmp(api_rec_error_string(API_REC_ERR_NONE), "none") == 0, "NONE -> none");
    ASSERT(strcmp(api_rec_error_string(API_REC_ERR_NETWORK), "network") == 0, "NETWORK -> network");
    ASSERT(strcmp(api_rec_error_string(API_REC_ERR_TIMEOUT), "timeout") == 0, "TIMEOUT -> timeout");
    ASSERT(strcmp(api_rec_error_string(API_REC_ERR_RATE_LIMIT), "rate_limit") == 0,
           "RATE_LIMIT -> rate_limit");
    ASSERT(strcmp(api_rec_error_string(API_REC_ERR_AUTH), "auth") == 0, "AUTH -> auth");
    ASSERT(strcmp(api_rec_error_string(API_REC_ERR_SERVER), "server") == 0, "SERVER -> server");
    ASSERT(strcmp(api_rec_error_string(API_REC_ERR_UNKNOWN), "unknown") == 0, "UNKNOWN -> unknown");
    ASSERT(strcmp(api_rec_error_string((api_rec_error_code_t)999), "unknown") == 0,
           "invalid -> unknown");
    PASS();
}

void test_degradation_string(void)
{
    TEST("Degradation level to string conversion");
    ASSERT(strcmp(api_rec_degradation_string(API_REC_DEGRADE_NONE), "none") == 0, "NONE -> none");
    ASSERT(strcmp(api_rec_degradation_string(API_REC_DEGRADE_LOWER_TIER), "lower_tier") == 0,
           "LOWER_TIER -> lower_tier");
    ASSERT(strcmp(api_rec_degradation_string(API_REC_DEGRADE_CACHE), "cache") == 0,
           "CACHE -> cache");
    ASSERT(strcmp(api_rec_degradation_string((api_rec_degradation_level_t)999), "unknown") == 0,
           "invalid -> unknown");
    PASS();
}

void test_full_workflow(void)
{
    TEST("Full API recovery workflow");
    api_rec_pool_t *pool = api_rec_pool_create("full_workflow");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-primary-key-001");
    api_rec_add_credential(pool, "sk-backup-key-002");
    api_rec_add_fallback_model(pool, "gpt-4", 10.0f, 1);
    api_rec_add_fallback_model(pool, "gpt-3.5-turbo", 1.0f, 2);
    api_rec_set_retry_config(pool, 3, 200, 2.0f, 0.15f);

    int dummy_cb = 42;
    api_rec_bind_circuit_breaker(pool, &dummy_cb);

    const char *cred = api_rec_next_credential(pool);
    ASSERT(cred != NULL, "should get credential");
    ASSERT(strcmp(cred, "sk-primary-key-001") == 0, "first credential should be primary");

    api_rec_mark_cred_success(pool);
    ASSERT(api_rec_cred_health(pool, 0) > 0.9, "primary health should be high");

    api_rec_degrade(pool);
    api_rec_degradation_level_t level = api_rec_current_level(pool);
    ASSERT(level == API_REC_DEGRADE_LOWER_TIER, "should be LOWER_TIER after degrade");

    const char *model = api_rec_current_model(pool);
    ASSERT(strcmp(model, "gpt-3.5-turbo") == 0, "should use second fallback after degrade");

    api_rec_upgrade(pool);
    level = api_rec_current_level(pool);
    ASSERT(level == API_REC_DEGRADE_NONE, "should be back to NONE");

    uint64_t total = 0, recovered = 0, failed = 0;
    double rate = 0.0;
    api_rec_get_stats(pool, &total, &recovered, &failed, &rate);
    ASSERT(total == 0, "total should be 0");

    api_rec_pool_destroy(pool);
    PASS();
}
