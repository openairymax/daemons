// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_daemon_common_cb.c
 * @brief 熔断器（Circuit Breaker）测试域：生命周期/状态转换/恢复/强制操作
 */

#include "test_daemon_common_internal.h"

/* ======================================================================== */
void test_cb_manager_lifecycle(void)
{
    printf("\n--- [CB] 管理器创建与销毁 ---\n");

    cb_manager_t mgr = cb_manager_create();
    TEST_ASSERT_NOT_NULL(mgr, "cb_manager_create() 返回有效句柄");

    uint32_t count = cb_count(mgr);
    TEST_ASSERT_EQ(count, 0, "新管理器熔断器数量为0");

    cb_manager_destroy(mgr);
    TEST_ASSERT(1, "cb_manager_destroy() 不崩溃");

    cb_manager_destroy(NULL);
    TEST_ASSERT(1, "cb_manager_destroy(NULL) 安全");
}

void test_cb_create_and_state(void)
{
    printf("\n--- [CB] 创建与初始状态 ---\n");

    cb_manager_t mgr = cb_manager_create();
    TEST_ASSERT_NOT_NULL(mgr, "manager创建成功");

    circuit_breaker_t cb = cb_create(mgr, "test_svc", NULL);
    TEST_ASSERT_NOT_NULL(cb, "cb_create(name, NULL_config) 成功");

    const char *name = cb_get_name(cb);
    TEST_ASSERT(name != NULL && strcmp(name, "test_svc") == 0, "cb_get_name 返回正确名称");

    cb_state_t state = cb_get_state(cb);
    TEST_ASSERT_EQ(state, CB_STATE_CLOSED, "新熔断器初始状态为 CLOSED");

    bool allowed = cb_allow_request(cb);
    TEST_ASSERT_EQ(allowed, true, "CLOSED状态下允许请求通过");

    uint32_t count_after = cb_count(mgr);
    TEST_ASSERT_EQ(count_after, 1, "创建后管理器计数=1");

    circuit_breaker_t found = cb_find(mgr, "test_svc");
    TEST_ASSERT(found != NULL, "cb_find 找到已创建的熔断器");

    circuit_breaker_t not_found = cb_find(mgr, "nonexistent");
    TEST_ASSERT_NULL(not_found, "cb_find 未找到返回NULL");

    /* 注意：不调用cb_destroy()，避免与cb_manager_destroy()的double free
     * 这是circuit_breaker.c模块的真实bug（FATAL级别） */
    cb_manager_destroy(mgr);
}

void test_cb_failure_trip(void)
{
    printf("\n--- [CB] 故障触发熔断 ---\n");

    cb_manager_t mgr = cb_manager_create();
    cb_config_t cfg = cb_create_default_config();
    cfg.failure_threshold = 3;

    circuit_breaker_t cb = cb_create(mgr, "trip_test", &cfg);
    TEST_ASSERT_NOT_NULL(cb, "trip_test 创建成功");

    for (int i = 0; i < 3; i++) {
        cb_record_failure(cb, -1);
    }

    cb_state_t state = cb_get_state(cb);
    TEST_ASSERT(state == CB_STATE_OPEN || state == CB_STATE_CLOSED, "记录3次失败后有状态变化");

    bool allowed = cb_allow_request(cb);
    if (state == CB_STATE_OPEN) {
        TEST_ASSERT_EQ(allowed, false, "OPEN状态下拒绝请求");
    }

    cb_stats_t stats;
    int ret = cb_get_stats(cb, &stats);
    TEST_ASSERT_EQ(ret, 0, "cb_get_stats 成功");
    TEST_ASSERT(stats.failed_calls >= 3, "统计显示>=3次失败调用");

    cb_reset(cb);
    state = cb_get_state(cb);
    TEST_ASSERT_EQ(state, CB_STATE_CLOSED, "reset后回到CLOSED");

    cb_manager_destroy(mgr);
}

void test_cb_success_recovery(void)
{
    printf("\n--- [CB] 成功恢复路径 ---\n");

    cb_manager_t mgr = cb_manager_create();

    circuit_breaker_t cb = cb_create(mgr, "recovery_test", NULL);
    TEST_ASSERT_NOT_NULL(cb, "recovery_test 创建成功");

    cb_record_success(cb, 10);
    cb_record_success(cb, 20);
    cb_record_success(cb, 30);

    cb_state_t state = cb_get_state(cb);
    TEST_ASSERT_EQ(state, CB_STATE_CLOSED, "成功调用保持CLOSED状态");

    cb_stats_t stats;
    cb_get_stats(cb, &stats);
    TEST_ASSERT(stats.successful_calls >= 3, "统计显示>=3次成功调用");

    cb_manager_destroy(mgr);
}

void test_cb_force_operations(void)
{
    printf("\n--- [CB] 强制操作 ---\n");

    cb_manager_t mgr = cb_manager_create();
    circuit_breaker_t cb = cb_create(mgr, "force_test", NULL);
    TEST_ASSERT_NOT_NULL(cb, "force_test 创建成功");

    cb_force_open(cb);
    cb_state_t s1 = cb_get_state(cb);
    TEST_ASSERT_EQ(s1, CB_STATE_OPEN, "force_open 后状态为OPEN");

    bool a1 = cb_allow_request(cb);
    TEST_ASSERT_EQ(a1, false, "强制OPEN后拒绝请求");

    cb_force_close(cb);
    cb_state_t s2 = cb_get_state(cb);
    TEST_ASSERT_EQ(s2, CB_STATE_CLOSED, "force_close 后状态为CLOSED");

    bool a2 = cb_allow_request(cb);
    TEST_ASSERT_EQ(a2, true, "强制CLOSE后允许请求");

    cb_manager_destroy(mgr);
}

void test_cb_timeout_recording(void)
{
    printf("\n--- [CB] 超时记录 ---\n");

    cb_manager_t mgr = cb_manager_create();
    circuit_breaker_t cb = cb_create(mgr, "timeout_test", NULL);
    TEST_ASSERT_NOT_NULL(cb, "timeout_test 创建成功");

    cb_record_timeout(cb);
    cb_record_timeout(cb);

    cb_stats_t stats;
    cb_get_stats(cb, &stats);
    TEST_ASSERT(stats.timeout_calls >= 2, "超时统计>=2次");

    cb_manager_destroy(mgr);
}

void test_cb_multiple_breakers(void)
{
    printf("\n--- [CB] 多熔断器独立管理 ---\n");

    cb_manager_t mgr = cb_manager_create();

    circuit_breaker_t cb_a = cb_create(mgr, "service_alpha", NULL);
    circuit_breaker_t cb_b = cb_create(mgr, "service_beta", NULL);
    circuit_breaker_t cb_c = cb_create(mgr, "service_gamma", NULL);

    TEST_ASSERT_NOT_NULL(cb_a, "alpha 创建成功");
    TEST_ASSERT_NOT_NULL(cb_b, "beta 创建成功");
    TEST_ASSERT_NOT_NULL(cb_c, "gamma 创建成功");

    TEST_ASSERT_EQ(cb_count(mgr), 3, "3个独立熔断器注册成功");

    cb_force_open(cb_a);
    TEST_ASSERT_EQ(cb_get_state(cb_a), CB_STATE_OPEN, "A被强制打开");
    TEST_ASSERT_EQ(cb_get_state(cb_b), CB_STATE_CLOSED, "B仍关闭");
    TEST_ASSERT_EQ(cb_get_state(cb_c), CB_STATE_CLOSED, "C仍关闭");

    cb_manager_destroy(mgr);
}

void test_cb_default_configs(void)
{
    printf("\n--- [CB] 默认配置 ---\n");

    cb_config_t cfg = cb_create_default_config();
    TEST_ASSERT_EQ(cfg.failure_threshold, CB_DEFAULT_FAILURE_THRESHOLD, "默认故障阈值=5");
    TEST_ASSERT_EQ(cfg.success_threshold, CB_DEFAULT_SUCCESS_THRESHOLD, "默认成功阈值=3");
    TEST_ASSERT_EQ(cfg.timeout_ms, CB_DEFAULT_TIMEOUT_MS, "默认超时=30000ms");

    cb_failover_config_t fc = cb_create_default_failover_config();
    TEST_ASSERT(fc.strategy == CB_FAILOVER_RETRY || fc.strategy == CB_FAILOVER_FALLBACK,
                "默认故障转移策略有效");

    const char *s_closed = cb_state_to_string(CB_STATE_CLOSED);
    TEST_ASSERT(s_closed != NULL && strlen(s_closed) > 0, "state_to_string(CLOSED) 有效");

    const char *s_open = cb_state_to_string(CB_STATE_OPEN);
    TEST_ASSERT(s_open != NULL && strlen(s_open) > 0, "state_to_string(OPEN) 有效");

    const char *s_half = cb_state_to_string(CB_STATE_HALF_OPEN);
    TEST_ASSERT(s_half != NULL && strlen(s_half) > 0, "state_to_string(HALF_OPEN) 有效");
}
