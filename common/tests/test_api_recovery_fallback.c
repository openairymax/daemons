// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_api_recovery_fallback.c - 降级模型/降级升级链路测试域
 */

#include "test_api_recovery_internal.h"

void test_add_fallback_model(void)
{
    TEST("Add fallback model");
    api_rec_pool_t *pool = api_rec_pool_create("fallback_test");
    ASSERT(pool != NULL, "create pool");

    int ret = api_rec_add_fallback_model(pool, "gpt-4", 10.0f, 1);
    ASSERT(ret == 0, "add first fallback should succeed");
    ASSERT(pool->fallback_count == 1, "fallback_count should be 1");
    ASSERT(strcmp(pool->fallback_models[0].model, "gpt-4") == 0, "model name should match");
    ASSERT(pool->fallback_models[0].available == true, "model should be available");

    ret = api_rec_add_fallback_model(pool, "gpt-3.5-turbo", 1.0f, 2);
    ASSERT(ret == 0, "add second fallback should succeed");
    ASSERT(pool->fallback_count == 2, "fallback_count should be 2");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_add_fallback_model_null_pool(void)
{
    TEST("Add fallback model to NULL pool");
    int ret = api_rec_add_fallback_model(NULL, "gpt-4", 1.0f, 1);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM");
    PASS();
}

void test_add_fallback_model_null_name(void)
{
    TEST("Add fallback model with NULL name");
    api_rec_pool_t *pool = api_rec_pool_create("fallback_null_test");
    ASSERT(pool != NULL, "create pool");

    int ret = api_rec_add_fallback_model(pool, NULL, 1.0f, 1);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_add_fallback_model_overflow(void)
{
    TEST("Add fallback model beyond max");
    api_rec_pool_t *pool = api_rec_pool_create("fallback_overflow");
    ASSERT(pool != NULL, "create pool");

    char model_name[64];
    int ret;
    for (size_t i = 0; i < API_REC_MAX_FALLBACK_MODELS; i++) {
        snprintf(model_name, sizeof(model_name), "fallback-model-%zu", i);
        ret = api_rec_add_fallback_model(pool, model_name, (float)i, (int)i);
        ASSERT(ret == 0, "add within limit should succeed");
    }
    ASSERT(pool->fallback_count == API_REC_MAX_FALLBACK_MODELS, "should be at max");

    ret = api_rec_add_fallback_model(pool, "one-too-many", 1.0f, 99);
    ASSERT(ret == AIRY_ERR_OVERFLOW, "should return OVERFLOW when full");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_current_model_primary(void)
{
    TEST("Current model when no degradation");
    api_rec_pool_t *pool = api_rec_pool_create("current_model_test");
    ASSERT(pool != NULL, "create pool");

    const char *model = api_rec_current_model(pool);
    ASSERT(model != NULL, "current model should not be NULL");
    ASSERT(strcmp(model, "primary") == 0, "should be 'primary' when no degradation");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_current_model_null_pool(void)
{
    TEST("Current model on NULL pool");
    const char *model = api_rec_current_model(NULL);
    ASSERT(model == NULL, "should return NULL for NULL pool");
    PASS();
}

void test_degrade_upgrade_cycle(void)
{
    TEST("Degrade and upgrade cycle");
    api_rec_pool_t *pool = api_rec_pool_create("degrade_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_fallback_model(pool, "fallback-gpt-4", 10.0f, 1);
    api_rec_add_fallback_model(pool, "fallback-gpt-3.5", 1.0f, 2);

    api_rec_degradation_level_t level = api_rec_current_level(pool);
    ASSERT(level == API_REC_DEGRADE_NONE, "initial level should be NONE");

    int ret = api_rec_degrade(pool);
    ASSERT(ret == 0, "first degrade should succeed");
    level = api_rec_current_level(pool);
    ASSERT(level == API_REC_DEGRADE_LOWER_TIER, "level should be LOWER_TIER after one degrade");

    const char *model = api_rec_current_model(pool);
    ASSERT(strcmp(model, "fallback-gpt-3.5") == 0,
           "should use second fallback model after one degrade");

    ret = api_rec_degrade(pool);
    ASSERT(ret == 0, "second degrade should succeed");
    level = api_rec_current_level(pool);
    ASSERT(level == API_REC_DEGRADE_CACHE, "level should be CACHE after second degrade");

    model = api_rec_current_model(pool);
    ASSERT(strcmp(model, "primary") == 0, "should return primary after all fallbacks exhausted");

    ret = api_rec_upgrade(pool);
    ASSERT(ret == 0, "first upgrade should succeed");
    level = api_rec_current_level(pool);
    ASSERT(level == API_REC_DEGRADE_LOWER_TIER, "level should be LOWER_TIER after upgrade");

    ret = api_rec_upgrade(pool);
    ASSERT(ret == 0, "second upgrade should succeed");
    level = api_rec_current_level(pool);
    ASSERT(level == API_REC_DEGRADE_NONE, "level should be NONE after full upgrade");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_degrade_null_pool(void)
{
    TEST("Degrade on NULL pool");
    int ret = api_rec_degrade(NULL);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM");
    PASS();
}

void test_upgrade_null_pool(void)
{
    TEST("Upgrade on NULL pool");
    int ret = api_rec_upgrade(NULL);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM");
    PASS();
}

void test_degrade_without_fallbacks(void)
{
    TEST("Degrade without fallback models");
    api_rec_pool_t *pool = api_rec_pool_create("no_fallback_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_degradation_level_t level = api_rec_current_level(pool);
    ASSERT(level == API_REC_DEGRADE_NONE, "initial level should be NONE");

    int ret = api_rec_degrade(pool);
    ASSERT(ret == 0, "degrade should succeed even without fallbacks");
    level = api_rec_current_level(pool);
    ASSERT(level == API_REC_DEGRADE_CACHE, "should go directly to CACHE when no fallbacks");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_upgrade_at_top(void)
{
    TEST("Upgrade when already at top level");
    api_rec_pool_t *pool = api_rec_pool_create("upgrade_top_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_add_fallback_model(pool, "fallback-gpt-4", 10.0f, 1);

    int ret = api_rec_upgrade(pool);
    ASSERT(ret == 0, "upgrade at top should succeed (no-op)");
    api_rec_degradation_level_t level = api_rec_current_level(pool);
    ASSERT(level == API_REC_DEGRADE_NONE, "level should still be NONE");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_current_level_null_pool(void)
{
    TEST("Current level on NULL pool");
    api_rec_degradation_level_t level = api_rec_current_level(NULL);
    ASSERT(level == API_REC_DEGRADE_NONE, "should return NONE for NULL pool");
    PASS();
}
