// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_api_recovery_pool.c - 池生命周期测试域
 */

#include "test_api_recovery_internal.h"

void test_pool_create_null_name(void)
{
    TEST("Pool create with NULL name");
    api_rec_pool_t *pool = api_rec_pool_create(NULL);
    ASSERT(pool != NULL, "create with NULL name should succeed");
    ASSERT(pool->cred_count == 0, "cred_count should be 0");
    ASSERT(pool->fallback_count == 0, "fallback_count should be 0");
    ASSERT(pool->current_level == API_REC_DEGRADE_NONE, "level should be NONE");
    api_rec_pool_destroy(pool);
    PASS();
}

void test_pool_create_with_name(void)
{
    TEST("Pool create with name");
    api_rec_pool_t *pool = api_rec_pool_create("test_pool");
    ASSERT(pool != NULL, "create should succeed");
    ASSERT(strcmp(pool->name, "test_pool") == 0, "name should match");
    ASSERT(pool->total_calls == 0, "total_calls should be 0");
    ASSERT(pool->recovered_calls == 0, "recovered_calls should be 0");
    ASSERT(pool->failed_calls == 0, "failed_calls should be 0");
    api_rec_pool_destroy(pool);
    PASS();
}

void test_pool_destroy_null_safe(void)
{
    TEST("Pool destroy NULL is safe");
    api_rec_pool_destroy(NULL);
    api_rec_pool_destroy(NULL);
    PASS();
}

void test_pool_multiple_create_destroy(void)
{
    TEST("Pool multiple create/destroy cycles");
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "cycle_%d", i);
        api_rec_pool_t *pool = api_rec_pool_create(name);
        ASSERT(pool != NULL, "create should succeed");
        ASSERT(api_rec_add_credential(pool, "sk-cyclic-key") == 0, "add credential should succeed");
        api_rec_pool_destroy(pool);
    }
    PASS();
}
