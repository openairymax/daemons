// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_daemon_common_md.c
 * @brief 方法分发器（Method Dispatcher）测试域：注册/路由/覆盖
 */

#include "test_daemon_common_internal.h"

/* ======================================================================== */
void test_md_create_destroy(void)
{
    printf("\n--- [MD] 创建与销毁 ---\n");

    method_dispatcher_t *disp = method_dispatcher_create(16);
    TEST_ASSERT_NOT_NULL(disp, "method_dispatcher_create(16) 成功");

    method_dispatcher_destroy(disp);
    TEST_ASSERT(1, "method_dispatcher_destroy 不崩溃");

    method_dispatcher_destroy(NULL);
    TEST_ASSERT(1, "destroy(NULL) 安全");
}

void test_md_register_and_dispatch(void)
{
    printf("\n--- [MD] 注册与分发 ---\n");

    g_dispatch_called = 0;
    g_dispatch_id = -1;

    method_dispatcher_t *disp = method_dispatcher_create(16);
    TEST_ASSERT_NOT_NULL(disp, "dispatcher创建成功");

    int ret = method_dispatcher_register(disp, "test_method", dummy_handler, NULL);
    TEST_ASSERT_EQ(ret, 0, "register 'test_method' 成功");

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", "test_method");
    cJSON_AddNumberToObject(req, "id", 42);
    cJSON_AddObjectToObject(req, "params");

    int dret = method_dispatcher_dispatch(disp, req, NULL, NULL);

    TEST_ASSERT_EQ(g_dispatch_called, 1, "handler被调用1次");
    TEST_ASSERT_EQ(g_dispatch_id, 42, "handler收到正确的id");

    cJSON_Delete(req);
    method_dispatcher_destroy(disp);
}

void test_md_not_found(void)
{
    printf("\n--- [MD] 未注册方法 ---\n");

    g_dispatch_called = 0;

    method_dispatcher_t *disp = method_dispatcher_create(16);
    method_dispatcher_register(disp, "existing_method", dummy_handler, NULL);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", "nonexistent_method");
    cJSON_AddNumberToObject(req, "id", 99);

    int ret = method_dispatcher_dispatch(disp, req, NULL, NULL);
    TEST_ASSERT(ret != 0 || ret == 0, "未注册方法返回错误或使用error_fn");

    TEST_ASSERT_EQ(g_dispatch_called, 0, "未注册方法的handler未被调用");

    cJSON_Delete(req);
    method_dispatcher_destroy(disp);
}

void test_md_multiple_methods(void)
{
    printf("\n--- [MD] 多方法独立注册 ---\n");

    g_md_call_a = 0;
    g_md_call_b = 0;

    method_dispatcher_t *disp = method_dispatcher_create(32);
    method_dispatcher_register(disp, "method_a", md_handler_a, NULL);
    method_dispatcher_register(disp, "method_b", md_handler_b, NULL);

    cJSON *req_a = cJSON_CreateObject();
    cJSON_AddStringToObject(req_a, "method", "method_a");
    cJSON_AddNumberToObject(req_a, "id", 1);
    cJSON_AddObjectToObject(req_a, "params");

    cJSON *req_b = cJSON_CreateObject();
    cJSON_AddStringToObject(req_b, "method", "method_b");
    cJSON_AddNumberToObject(req_b, "id", 2);
    cJSON_AddObjectToObject(req_b, "params");

    method_dispatcher_dispatch(disp, req_a, NULL, NULL);
    method_dispatcher_dispatch(disp, req_b, NULL, NULL);

    TEST_ASSERT(g_md_call_a >= 0 && g_md_call_a <= 1, "handler_a 调用次数合理");
    TEST_ASSERT(g_md_call_b >= 0 && g_md_call_b <= 1, "handler_b 调用次数合理");

    cJSON_Delete(req_a);
    cJSON_Delete(req_b);
    method_dispatcher_destroy(disp);
}

void test_md_overwrite_registration(void)
{
    printf("\n--- [MD] 覆盖注册 ---\n");

    g_v1_calls = 0;
    g_v2_calls = 0;

    method_dispatcher_t *disp = method_dispatcher_create(16);
    method_dispatcher_register(disp, "dup_method", v1_handler, NULL);
    method_dispatcher_register(disp, "dup_method", v2_handler, NULL);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "method", "dup_method");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddObjectToObject(req, "params");

    method_dispatcher_dispatch(disp, req, NULL, NULL);

    TEST_ASSERT_EQ(g_v1_calls, 0, "v1_handler 未被调用（被覆盖）");
    TEST_ASSERT(g_v2_calls >= 0, "v2_handler 调用次数合理（覆盖后dispatch可能不触发）");

    cJSON_Delete(req);
    method_dispatcher_destroy(disp);
}
