// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * test_api_recovery_config.c - 重试配置/熔断绑定/统计测试域
 */

#include "test_api_recovery_internal.h"

void test_set_retry_config(void)
{
    TEST("Set retry configuration");
    api_rec_pool_t *pool = api_rec_pool_create("retry_config_test");
    ASSERT(pool != NULL, "create pool");

    int ret = api_rec_set_retry_config(pool, 3, 500, 2.0f, 0.1f);
    ASSERT(ret == 0, "set retry config should succeed");
    ASSERT(pool->max_retries == 3, "max_retries should be 3");
    ASSERT(pool->base_delay_ms == 500, "base_delay_ms should be 500");
    ASSERT(pool->backoff_factor == 2.0f, "backoff_factor should be 2.0");
    ASSERT(pool->jitter_ratio == 0.1f, "jitter_ratio should be 0.1");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_set_retry_config_null_pool(void)
{
    TEST("Set retry config on NULL pool");
    int ret = api_rec_set_retry_config(NULL, 3, 500, 2.0f, 0.1f);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM");
    PASS();
}

void test_set_retry_config_edge_values(void)
{
    TEST("Set retry config edge values");
    api_rec_pool_t *pool = api_rec_pool_create("retry_edge_test");
    ASSERT(pool != NULL, "create pool");

    int ret = api_rec_set_retry_config(pool, 0, 0, 0.0f, 0.0f);
    ASSERT(ret == 0, "set zero values should succeed");
    ASSERT(pool->max_retries == API_REC_MAX_RETRY, "zero max_retries should fallback to default");
    ASSERT(pool->base_delay_ms == API_REC_DEFAULT_BASE_DELAY_MS,
           "zero delay should fallback to default");
    ASSERT(pool->backoff_factor == 2.0f, "zero backoff should fallback to 2.0");
    ASSERT(pool->jitter_ratio == 0.0f, "zero jitter should be accepted");

    ret = api_rec_set_retry_config(pool, 10, 10000, 3.0f, 0.5f);
    ASSERT(ret == 0, "set large values should succeed");
    ASSERT(pool->max_retries == 10, "max_retries should be 10");
    ASSERT(pool->base_delay_ms == 10000, "base_delay_ms should be 10000");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_bind_circuit_breaker(void)
{
    TEST("Bind circuit breaker");
    api_rec_pool_t *pool = api_rec_pool_create("cb_bind_test");
    ASSERT(pool != NULL, "create pool");

    int dummy_cb = 42;
    int ret = api_rec_bind_circuit_breaker(pool, &dummy_cb);
    ASSERT(ret == 0, "bind circuit breaker should succeed");
    ASSERT(pool->cb_breaker == &dummy_cb, "cb_breaker should point to dummy");

    api_rec_bind_circuit_breaker(pool, NULL);
    ASSERT(pool->cb_breaker == NULL, "unbind should set cb_breaker to NULL");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_bind_circuit_breaker_null_pool(void)
{
    TEST("Bind circuit breaker to NULL pool");
    int dummy_cb = 42;
    int ret = api_rec_bind_circuit_breaker(NULL, &dummy_cb);
    ASSERT(ret == AIRY_ERR_INVALID_PARAM, "should return INVALID_PARAM");
    PASS();
}

void test_get_stats(void)
{
    TEST("Get statistics");
    api_rec_pool_t *pool = api_rec_pool_create("stats_test");
    ASSERT(pool != NULL, "create pool");

    uint64_t total = 0, recovered = 0, failed = 0;
    double rate = 0.0;

    api_rec_get_stats(pool, &total, &recovered, &failed, &rate);
    ASSERT(total == 0, "initial total should be 0");
    ASSERT(recovered == 0, "initial recovered should be 0");
    ASSERT(failed == 0, "initial failed should be 0");
    ASSERT(rate == 0.0, "initial rate should be 0.0");

    api_rec_pool_destroy(pool);
    PASS();
}

void test_get_stats_null_pool(void)
{
    TEST("Get statistics on NULL pool");
    uint64_t total = 99, recovered = 99, failed = 99;
    double rate = 99.0;

    api_rec_get_stats(NULL, &total, &recovered, &failed, &rate);
    ASSERT(total == 99, "total should be unchanged on NULL pool");
    ASSERT(recovered == 99, "recovered should be unchanged");
    ASSERT(failed == 99, "failed should be unchanged");
    ASSERT(rate == 99.0, "rate should be unchanged");

    PASS();
}

void test_get_stats_null_output(void)
{
    TEST("Get statistics with NULL output pointers");
    api_rec_pool_t *pool = api_rec_pool_create("stats_null_test");
    ASSERT(pool != NULL, "create pool");

    api_rec_get_stats(pool, NULL, NULL, NULL, NULL);

    uint64_t total = 0;
    double rate = 0.0;
    api_rec_get_stats(pool, &total, NULL, NULL, &rate);
    ASSERT(total == 0, "total should be populated");
    ASSERT(rate == 0.0, "rate should be populated");

    api_rec_pool_destroy(pool);
    PASS();
}
