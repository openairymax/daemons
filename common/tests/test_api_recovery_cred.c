// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_api_recovery_cred.c - 凭据增删/轮转/标记测试域
 */

#include "test_api_recovery_internal.h"

void test_add_credential(void)
{
    TEST("Add credential");
    api_rec_pool_t *pool = api_rec_pool_create("cred_test");
    ASSERT(pool != NULL, "create pool");

    int ret = api_rec_add_credential(pool, "sk-api-key-001");
    ASSERT(ret == 0, "add first credential should succeed");
    ASSERT(pool->cred_count == 1, "cred_count should be 1");
    ASSERT(pool->credentials[0].health_score == 1.0, "initial health should be 1.0");
    ASSERT(pool->credentials[0].is_valid == true, "credential should be valid");

    ret = api_rec_add_credential(pool, "sk-api-key-002");
    ASSERT(ret == 0, "add second credential should succeed");
    ASSERT(pool->cred_count == 2, "cred_count should be 2");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_add_credential_null_pool(void)
{
    TEST("Add credential to NULL pool");
    int ret = api_rec_add_credential(NULL, "sk-key");
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM");
    PASS();
}

void test_add_credential_null_key(void)
{
    TEST("Add credential with NULL key");
    api_rec_pool_t *pool = api_rec_pool_create("null_key_test");
    ASSERT(pool != NULL, "create pool");

    int ret = api_rec_add_credential(pool, NULL);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_add_credential_overflow(void)
{
    TEST("Add credential beyond max limit");
    api_rec_pool_t *pool = api_rec_pool_create("overflow_test");
    ASSERT(pool != NULL, "create pool");

    char key[64];
    int ret;
    for (size_t i = 0; i < API_REC_MAX_CREDENTIALS; i++) {
        snprintf(key, sizeof(key), "sk-overflow-key-%zu", i);
        ret = api_rec_add_credential(pool, key);
        ASSERT(ret == 0, "add within limit should succeed");
    }
    ASSERT(pool->cred_count == API_REC_MAX_CREDENTIALS, "cred_count should be at max");

    ret = api_rec_add_credential(pool, "sk-one-too-many");
    ASSERT(ret == AIRY_ERR_OVERFLOW, "should return OVERFLOW when full");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_remove_credential(void)
{
    TEST("Remove credential");
    api_rec_pool_t *pool = api_rec_pool_create("remove_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-key-001");
    api_rec_add_credential(pool, "sk-key-002");
    api_rec_add_credential(pool, "sk-key-003");
    ASSERT(pool->cred_count == 3, "should have 3 credentials");

    int ret = api_rec_remove_credential(pool, 1);
    ASSERT(ret == 0, "remove middle credential should succeed");
    ASSERT(pool->cred_count == 2, "cred_count should be 2");

    ret = api_rec_remove_credential(pool, 0);
    ASSERT(ret == 0, "remove first credential should succeed");
    ASSERT(pool->cred_count == 1, "cred_count should be 1");

    ret = api_rec_remove_credential(pool, 0);
    ASSERT(ret == 0, "remove last credential should succeed");
    ASSERT(pool->cred_count == 0, "cred_count should be 0");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_remove_credential_null_pool(void)
{
    TEST("Remove credential from NULL pool");
    int ret = api_rec_remove_credential(NULL, 0);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM");
    PASS();
}

void test_remove_credential_out_of_range(void)
{
    TEST("Remove credential out of range");
    api_rec_pool_t *pool = api_rec_pool_create("oor_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-key-001");
    ASSERT(pool->cred_count == 1, "should have 1 credential");

    int ret = api_rec_remove_credential(pool, 5);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM for out of range");

    ret = api_rec_remove_credential(pool, 1);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM for index == count");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_next_credential_rotation(void)
{
    TEST("Next credential rotation");
    api_rec_pool_t *pool = api_rec_pool_create("rotation_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-key-A");
    api_rec_add_credential(pool, "sk-key-B");
    api_rec_add_credential(pool, "sk-key-C");

    const char *cred1 = api_rec_next_credential(pool);
    ASSERT(cred1 != NULL, "first credential should not be NULL");
    ASSERT(strcmp(cred1, "sk-key-A") == 0, "first should be key-A");

    const char *cred2 = api_rec_next_credential(pool);
    ASSERT(cred2 != NULL, "second credential should not be NULL");
    ASSERT(strcmp(cred2, "sk-key-B") == 0, "second should be key-B");

    const char *cred3 = api_rec_next_credential(pool);
    ASSERT(cred3 != NULL, "third credential should not be NULL");
    ASSERT(strcmp(cred3, "sk-key-C") == 0, "third should be key-C");

    const char *cred4 = api_rec_next_credential(pool);
    ASSERT(cred4 != NULL, "wraparound should not be NULL");
    ASSERT(strcmp(cred4, "sk-key-A") == 0, "wraparound should be key-A");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_next_credential_null_pool(void)
{
    TEST("Next credential on NULL pool");
    const char *cred = api_rec_next_credential(NULL);
    ASSERT(cred == NULL, "should return NULL for NULL pool");
    PASS();
}

void test_next_credential_empty_pool(void)
{
    TEST("Next credential on empty pool");
    api_rec_pool_t *pool = api_rec_pool_create("empty_cred_test");
    ASSERT(pool != NULL, "create pool");

    const char *cred = api_rec_next_credential(pool);
    ASSERT(cred == NULL, "should return NULL for empty pool");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_mark_cred_success(void)
{
    TEST("Mark credential success");
    api_rec_pool_t *pool = api_rec_pool_create("mark_success_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-mark-success-001");
    api_rec_next_credential(pool);

    int ret = api_rec_mark_cred_success(pool);
    ASSERT(ret == 0, "mark success should succeed");

    double health = api_rec_cred_health(pool, 0);
    ASSERT(health > 0.9, "health should remain high after success");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_mark_cred_failure(void)
{
    TEST("Mark credential failure");
    api_rec_pool_t *pool = api_rec_pool_create("mark_failure_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_credential(pool, "sk-mark-fail-001");
    api_rec_next_credential(pool);

    int ret = api_rec_mark_cred_failure(pool, API_REC_ERR_NETWORK);
    ASSERT(ret == 0, "mark failure should succeed");

    double health = api_rec_cred_health(pool, 0);
    ASSERT(health < 1.0, "health should decrease after failure");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_mark_cred_success_null_pool(void)
{
    TEST("Mark cred success on NULL pool");
    int ret = api_rec_mark_cred_success(NULL);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM");
    PASS();
}

void test_mark_cred_failure_null_pool(void)
{
    TEST("Mark cred failure on NULL pool");
    int ret = api_rec_mark_cred_failure(NULL, API_REC_ERR_NETWORK);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM");
    PASS();
}

void test_mark_cred_empty_pool(void)
{
    TEST("Mark cred on empty pool");
    api_rec_pool_t *pool = api_rec_pool_create("empty_mark_test");
    ASSERT(pool != NULL, "create pool");

    int ret = api_rec_mark_cred_success(pool);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "mark success on empty should fail");

    ret = api_rec_mark_cred_failure(pool, API_REC_ERR_NETWORK);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "mark failure on empty should fail");

    api_rec_pool_destroy(pool);
    PASS();
}
