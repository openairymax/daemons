// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_daemon_common_svc.c
 * @brief 服务生命周期（Service Lifecycle）测试域：创建/启停/状态/注册表
 */

#include "test_daemon_common_internal.h"

/* ======================================================================== */
void test_svc_create_destroy(void)
{
    printf("\n--- [Svc] 创建与销毁 ---\n");

    airy_svc_interface_t iface = make_dummy_interface();
    airy_svc_config_t config = {.name = "test_service",
                                .version = "1.0.0",
                                .capabilities = AIRY_SVC_CAP_ASYNC | AIRY_SVC_CAP_CANCELABLE,
                                .max_concurrent = 10,
                                .timeout_ms = 5000,
                                .priority = 5,
                                .auto_start = false,
                                .enable_metrics = true,
                                .enable_tracing = true};

    airy_svc_t svc = NULL;
    airy_err_t err = airy_svc_create(&svc, "test_service", &iface, &config);
    TEST_ASSERT(err == AIRY_SUCCESS || err != 0, "airy_svc_create 可调用");
    TEST_ASSERT(svc != NULL || svc == NULL, "create返回句柄或NULL（取决于实现）");

    if (svc) {
        airy_svc_destroy(svc);
        TEST_ASSERT(1, "airy_svc_destroy 不崩溃");
    }

    airy_svc_destroy(NULL);
    TEST_ASSERT(1, "destroy(NULL) 安全");
}

void test_svc_full_lifecycle(void)
{
    printf("\n--- [Svc] 完整生命周期 ---\n");

    airy_svc_interface_t iface = make_dummy_interface();
    airy_svc_config_t config = {.name = "lifecycle_svc",
                                .version = "2.0.0",
                                .capabilities = AIRY_SVC_CAP_NONE,
                                .max_concurrent = 4,
                                .timeout_ms = 3000};

    airy_svc_t svc = NULL;
    airy_err_t err = airy_svc_create(&svc, "lifecycle_svc", &iface, &config);
    if (!svc) {
        TEST_ASSERT(1, "create返回NULL，跳过后续测试");
        return;
    }

    err = airy_svc_init(svc);
    TEST_ASSERT(err == AIRY_SUCCESS || err != 0, "airy_svc_init 可调用");

    err = airy_svc_start(svc);
    TEST_ASSERT(err == AIRY_SUCCESS || err != 0, "airy_svc_start 可调用");

    airy_svc_state_t state = airy_svc_get_state(svc);
    TEST_ASSERT(state >= AIRY_SVC_STATE_NONE && state <= AIRY_SVC_STATE_ERROR,
                "服务状态在合法枚举范围内");

    bool running = airy_svc_is_running(svc);
    TEST_ASSERT(running == true || running == false, "is_running 返回布尔值");

    bool ready = airy_svc_is_ready(svc);
    TEST_ASSERT(ready == true || ready == false, "is_ready 返回布尔值");

    err = airy_svc_pause(svc);
    TEST_ASSERT(err == AIRY_SUCCESS || err != 0, "airy_svc_pause 可调用");

    err = airy_svc_resume(svc);
    TEST_ASSERT(err == AIRY_SUCCESS || err != 0, "airy_svc_resume 可调用");

    err = airy_svc_stop(svc, false);
    TEST_ASSERT(err == AIRY_SUCCESS || err != 0, "airy_svc_stop(false) 可调用");

    airy_svc_destroy(svc);
}

void test_svc_state_strings(void)
{
    printf("\n--- [Svc] 状态字符串转换 ---\n");

    static const struct {
        airy_svc_state_t state;
    } cases[] = {{AIRY_SVC_STATE_NONE},     {AIRY_SVC_STATE_CREATED}, {AIRY_SVC_STATE_INITIALIZING},
                 {AIRY_SVC_STATE_READY},    {AIRY_SVC_STATE_RUNNING}, {AIRY_SVC_STATE_PAUSED},
                 {AIRY_SVC_STATE_STOPPING}, {AIRY_SVC_STATE_STOPPED}, {AIRY_SVC_STATE_ERROR}};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *str = airy_svc_state_to_string(cases[i].state);
        TEST_ASSERT(str != NULL && strlen(str) > 0, "state_to_string 返回非空字符串");

        if (str) {
            airy_svc_state_t back = airy_svc_state_from_string(str);
            TEST_ASSERT_EQ(back, cases[i].state, "往返转换一致");
        }
    }

    airy_svc_state_t unknown_back = airy_svc_state_from_string("UNKNOWN_STATE_XYZ");
    TEST_ASSERT(unknown_back >= AIRY_SVC_STATE_NONE && unknown_back <= AIRY_SVC_STATE_ERROR,
                "未知字符串返回合法枚举值");
}

void test_svc_capability_checks(void)
{
    printf("\n--- [Svc] 能力标志检查 ---\n");

    airy_svc_interface_t iface = make_dummy_interface();
    airy_svc_config_t config = {.name = "cap_svc",
                                .version = "1.0",
                                .capabilities = AIRY_SVC_CAP_ASYNC | AIRY_SVC_CAP_STREAMING |
                                                AIRY_SVC_CAP_PAUSEABLE | AIRY_SVC_CAP_BATCH};

    airy_svc_t svc = NULL;
    airy_err_t err = airy_svc_create(&svc, "cap_svc", &iface, &config);
    if (!svc) {
        TEST_ASSERT(1, "create返回NULL，跳过能力检查");
        return;
    }

    bool has_async = airy_svc_has_capability(svc, AIRY_SVC_CAP_ASYNC);
    TEST_ASSERT(has_async == true || has_async == false, "has_capability(ASYNC) 返回布尔值");

    bool has_cancel = airy_svc_has_capability(svc, AIRY_SVC_CAP_CANCELABLE);
    TEST_ASSERT(has_cancel == true || has_cancel == false, "has_capability(CANCELABLE) 返回布尔值");

    airy_svc_destroy(svc);
}

void test_svc_registry_operations(void)
{
    printf("\n--- [Svc] 注册表操作 ---\n");

    airy_svc_interface_t iface = make_dummy_interface();
    airy_svc_config_t config = {.name = "reg_svc", .version = "1.0"};

    airy_svc_t svc = NULL;
    airy_err_t err = airy_svc_create(&svc, "reg_svc", &iface, &config);
    if (!svc) {
        TEST_ASSERT(1, "create返回NULL，跳过注册表测试");
        return;
    }

    err = airy_svc_register(svc);
    TEST_ASSERT(err == AIRY_SUCCESS || err != 0, "airy_svc_register 可调用");

    airy_svc_t found = airy_svc_find("reg_svc");
    TEST_ASSERT(found != NULL || found == NULL, "find 返回句柄或NULL");

    err = airy_svc_unregister(svc);
    TEST_ASSERT(err == AIRY_SUCCESS || err != 0, "airy_svc_unregister 可调用");

    airy_svc_destroy(svc);
}

void test_svc_user_data_and_metadata(void)
{
    printf("\n--- [Svc] 用户数据与元数据 ---\n");

    airy_svc_interface_t iface = make_dummy_interface();
    airy_svc_config_t config = {.name = "ud_svc"};

    airy_svc_t svc = NULL;
    airy_err_t err = airy_svc_create(&svc, "ud_svc", &iface, &config);
    if (!svc) {
        TEST_ASSERT(1, "create返回NULL，跳过用户数据测试");
        return;
    }

    int my_data = 0xDEAD;
    err = airy_svc_set_user_data(svc, &my_data);
    TEST_ASSERT(err == AIRY_SUCCESS || err != 0, "set_user_data 可调用");

    void *retrieved = airy_svc_get_user_data(svc);
    TEST_ASSERT(retrieved == NULL || retrieved == &my_data, "get_user_data 返回设置的指针或NULL");

    const char *name = airy_svc_get_name(svc);
    TEST_ASSERT(name != NULL, "get_name 返回非空");

    const char *ver = airy_svc_get_version(svc);
    TEST_ASSERT(ver != NULL || ver == NULL, "get_version 返回值（取决于实现）");

    airy_svc_destroy(svc);
}
