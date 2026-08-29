// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_api_recovery_health.c - 凭据健康度测试域（查询/衰减/恢复/禁用）
 */

#include "test_api_recovery_internal.h"

void test_cred_health(void)
{
    TEST("Credential health query");
    api_rec_pool_t *pool = api_rec_pool_create("health_query_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-health-001");
    api_rec_add_credential(pool, "sk-health-002");

    double health0 = api_rec_cred_health(pool, 0);
    ASSERT(health0 == 1.0, "initial health should be 1.0");

    double health1 = api_rec_cred_health(pool, 1);
    ASSERT(health1 == 1.0, "initial health should be 1.0");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_cred_health_null_pool(void)
{
    TEST("Credential health on NULL pool");
    double health = api_rec_cred_health(NULL, 0);
    ASSERT(health < 0.0, "should return negative for NULL pool");
    PASS();
}

void test_cred_health_out_of_range(void)
{
    TEST("Credential health out of range");
    api_rec_pool_t *pool = api_rec_pool_create("health_oor_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-health-oor-001");

    double health = api_rec_cred_health(pool, 5);
    ASSERT(health < 0.0, "should return negative for out of range");

    health = api_rec_cred_health(pool, 1);
    ASSERT(health < 0.0, "should return negative for index == count");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_credential_health_decay_after_failures(void)
{
    TEST("Credential health decays after multiple failures");
    api_rec_pool_t *pool = api_rec_pool_create("health_decay_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-decay-001");
    api_rec_next_credential(pool);

    double initial_health = api_rec_cred_health(pool, 0);
    ASSERT(initial_health == 1.0, "initial health should be 1.0");

    api_rec_mark_cred_failure(pool, API_REC_ERR_NETWORK);
    double health1 = api_rec_cred_health(pool, 0);
    ASSERT(health1 < 1.0, "health should decrease after first failure");

    api_rec_next_credential(pool);
    api_rec_mark_cred_failure(pool, API_REC_ERR_NETWORK);
    double health2 = api_rec_cred_health(pool, 0);
    ASSERT(health2 < health1, "health should decrease further after second failure");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_credential_health_recovery_on_success(void)
{
    TEST("Credential health recovers after success");
    api_rec_pool_t *pool = api_rec_pool_create("health_recovery_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-recover-001");
    api_rec_next_credential(pool);
    api_rec_mark_cred_failure(pool, API_REC_ERR_NETWORK);
    double health_after_fail = api_rec_cred_health(pool, 0);

    api_rec_next_credential(pool);
    api_rec_mark_cred_success(pool);
    double health_after_success = api_rec_cred_health(pool, 0);
    ASSERT(health_after_success > health_after_fail, "health should improve after success");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_credential_auth_failure_disables(void)
{
    TEST("Auth failure disables credential");
    api_rec_pool_t *pool = api_rec_pool_create("auth_disable_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-auth-disable-001");
    api_rec_next_credential(pool);

    int ret = api_rec_mark_cred_failure(pool, API_REC_ERR_AUTH);
    ASSERT(ret == 0, "mark auth failure should succeed");

    ASSERT(pool->credentials[0].is_valid == false,
           "credential should be disabled after auth failure");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_remove_credential_adjusts_index(void)
{
    TEST("Remove credential adjusts cred_index");
    api_rec_pool_t *pool = api_rec_pool_create("remove_index_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-rm-idx-A");
    api_rec_add_credential(pool, "sk-rm-idx-B");
    api_rec_add_credential(pool, "sk-rm-idx-C");

    api_rec_next_credential(pool);
    api_rec_next_credential(pool);
    api_rec_next_credential(pool);

    api_rec_remove_credential(pool, pool->cred_count - 1);
    api_rec_next_credential(pool);

    api_rec_pool_destroy(pool);
    PASS();
}
