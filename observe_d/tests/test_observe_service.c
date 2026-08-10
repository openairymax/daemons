// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file test_observe_service.c
 * @brief Observe 服务单元测试（L2 方法集：record_metric / query_metrics / get_metrics）
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * observe_d 的实现全部位于 src/main.c（含 main()）。本测试将 main 重命名后
 * include 源文件，直接复用全部 static 实现（真实代码，禁止桩函数）。
 */

#define main observe_d_daemon_main
#include "../src/main.c"
#undef main

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

/* ---------- 辅助 ---------- */

static void test_svc_setup(observe_d_service_t *svc)
{
    int rc = observe_d_init(svc, 0, "/tmp/observe_d_test.sock");
    assert(rc == AIRY_SUCCESS);
}

static void test_svc_teardown(observe_d_service_t *svc)
{
    /* observe_d_init 未创建任何 socket，memset 后 http_fd/server_fd 为 0（stdin），
     * destroy 前必须置为 AIRY_INVALID_SOCKET，避免 close(0) 关闭标准输入。 */
    svc->http_fd = AIRY_INVALID_SOCKET;
    svc->server_fd = AIRY_INVALID_SOCKET;
    observe_d_destroy(svc);
}

/* ---------- 用例 1：record_metric 后指标可查询 ---------- */

static void test_record_metric_queryable(void)
{
    printf("  test_record_metric_queryable...\n");
    observe_d_service_t svc = {0};
    test_svc_setup(&svc);

    int rc = observe_d_record_metric(&svc, "test_latency_ms", 12.5, "ms",
                                     OBSERVE_METRIC_GAUGE);
    assert(rc == AIRY_SUCCESS);

    observe_metric_t *m = observe_d_find_or_create_metric(&svc, "test_latency_ms");
    assert(m != NULL);
    assert(fabs(m->value - 12.5) < 1e-9);
    assert(m->type == OBSERVE_METRIC_GAUGE);
    assert(m->unit && strcmp(m->unit, "ms") == 0);

    test_svc_teardown(&svc);
    printf("    PASSED\n");
}

/* ---------- 用例 2：同名指标重复记录的行为 ---------- */

static void test_counter_accumulate(void)
{
    printf("  test_counter_accumulate...\n");
    observe_d_service_t svc = {0};
    test_svc_setup(&svc);

    observe_d_record_metric(&svc, "test_counter", 1.0, "count", OBSERVE_METRIC_COUNTER);
    observe_d_record_metric(&svc, "test_counter", 2.0, "count", OBSERVE_METRIC_COUNTER);
    observe_d_record_metric(&svc, "test_counter", 3.5, "count", OBSERVE_METRIC_COUNTER);

    observe_metric_t *m = observe_d_find_or_create_metric(&svc, "test_counter");
    assert(m != NULL);
    assert(fabs(m->value - 6.5) < 1e-9); /* 1.0 + 2.0 + 3.5 累加 */
    assert(m->type == OBSERVE_METRIC_COUNTER);

    test_svc_teardown(&svc);
    printf("    PASSED\n");
}

static void test_gauge_overwrite(void)
{
    printf("  test_gauge_overwrite...\n");
    observe_d_service_t svc = {0};
    test_svc_setup(&svc);

    observe_d_record_metric(&svc, "test_gauge", 1.0, NULL, OBSERVE_METRIC_GAUGE);
    observe_d_record_metric(&svc, "test_gauge", 42.0, NULL, OBSERVE_METRIC_GAUGE);

    observe_metric_t *m = observe_d_find_or_create_metric(&svc, "test_gauge");
    assert(m != NULL);
    assert(fabs(m->value - 42.0) < 1e-9); /* gauge 覆盖旧值 */
    assert(m->type == OBSERVE_METRIC_GAUGE);

    test_svc_teardown(&svc);
    printf("    PASSED\n");
}

/* ---------- 用例 3：JSON-RPC 方法分发响应为合法 JSON ---------- */

#ifndef _WIN32
/* 通过 socketpair 模拟 IPC 客户端：发送 JSON-RPC 请求并读取响应 */
static char *rpc_roundtrip(observe_d_service_t *svc, const char *method,
                           const char *params_json, int id)
{
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", (double)id);
    cJSON_AddStringToObject(root, "method", method);
    if (params_json) {
        cJSON *params = cJSON_Parse(params_json);
        assert(params != NULL);
        cJSON_AddItemToObject(root, "params", params);
    }
    char *req = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    assert(req != NULL);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ssize_t wr = write(fds[1], req, strlen(req));
    assert(wr == (ssize_t)strlen(req));
    cJSON_free(req);

    observe_d_handle_request(svc, (airy_sock_t)fds[0]);

    /* 服务端已关闭 fds[0]；循环读取直到 EOF */
    char buf[16384];
    size_t off = 0;
    for (;;) {
        ssize_t r = read(fds[1], buf + off, sizeof(buf) - 1 - off);
        if (r <= 0)
            break;
        off += (size_t)r;
        if (off >= sizeof(buf) - 1)
            break;
    }
    close(fds[1]);
    buf[off] = '\0';
    return AIRY_STRDUP(buf);
}

/* 校验响应为合法 JSON-RPC 2.0 成功响应（jsonrpc/id/result），返回 root */
static cJSON *rpc_expect_success(const char *resp, int id)
{
    cJSON *root = cJSON_Parse(resp);
    assert(root != NULL); /* 方法分发响应必须为合法 JSON */
    cJSON *jv = cJSON_GetObjectItem(root, "jsonrpc");
    assert(cJSON_IsString(jv) && strcmp(jv->valuestring, "2.0") == 0);
    cJSON *idj = cJSON_GetObjectItem(root, "id");
    assert(cJSON_IsNumber(idj) && idj->valueint == id);
    cJSON *res = cJSON_GetObjectItem(root, "result");
    assert(cJSON_IsObject(res));
    return root;
}

static void test_jsonrpc_record_metric(void)
{
    printf("  test_jsonrpc_record_metric...\n");
    observe_d_service_t svc = {0};
    test_svc_setup(&svc);

    char *resp = rpc_roundtrip(&svc, "record_metric",
                               "{\"name\":\"rpc_counter\",\"value\":5.0,"
                               "\"type\":\"counter\",\"unit\":\"count\"}",
                               7);
    assert(resp != NULL);
    cJSON *root = rpc_expect_success(resp, 7);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *status = cJSON_GetObjectItem(result, "status");
    assert(cJSON_IsString(status) && strcmp(status->valuestring, "recorded") == 0);
    cJSON_Delete(root);
    AIRY_FREE(resp);

    /* 响应是分发的唯一途径：指标必须真实入库 */
    observe_metric_t *m = observe_d_find_or_create_metric(&svc, "rpc_counter");
    assert(m != NULL);
    assert(fabs(m->value - 5.0) < 1e-9);
    assert(m->type == OBSERVE_METRIC_COUNTER);
    assert(m->unit && strcmp(m->unit, "count") == 0);

    test_svc_teardown(&svc);
    printf("    PASSED\n");
}

static void test_jsonrpc_record_metric_invalid(void)
{
    printf("  test_jsonrpc_record_metric_invalid...\n");
    observe_d_service_t svc = {0};
    test_svc_setup(&svc);

    /* 缺少 name → 必须返回 JSON-RPC 错误响应（合法 JSON） */
    char *resp = rpc_roundtrip(&svc, "record_metric", "{\"value\":1.0}", 8);
    assert(resp != NULL);
    cJSON *root = cJSON_Parse(resp);
    assert(root != NULL);
    cJSON *jv = cJSON_GetObjectItem(root, "jsonrpc");
    assert(cJSON_IsString(jv) && strcmp(jv->valuestring, "2.0") == 0);
    cJSON *err = cJSON_GetObjectItem(root, "error");
    assert(cJSON_IsObject(err));
    cJSON *code = cJSON_GetObjectItem(err, "code");
    assert(cJSON_IsNumber(code) && code->valueint == JSONRPC_INVALID_PARAMS);
    cJSON_Delete(root);
    AIRY_FREE(resp);

    test_svc_teardown(&svc);
    printf("    PASSED\n");
}

static void test_jsonrpc_query_metrics(void)
{
    printf("  test_jsonrpc_query_metrics...\n");
    observe_d_service_t svc = {0};
    test_svc_setup(&svc);

    observe_d_record_metric(&svc, "rpc_gauge", 3.14, "bytes", OBSERVE_METRIC_GAUGE);

    /* 不带 name：返回全部指标（名称、值、类型、单位） */
    char *resp = rpc_roundtrip(&svc, "query_metrics", NULL, 9);
    assert(resp != NULL);
    cJSON *root = rpc_expect_success(resp, 9);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *count = cJSON_GetObjectItem(result, "count");
    assert(cJSON_IsNumber(count) && count->valueint == (double)svc.metric_count);
    cJSON *arr = cJSON_GetObjectItem(result, "metrics");
    assert(cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == (int)count->valueint);
    int found = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        cJSON *namej = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(namej) && strcmp(namej->valuestring, "rpc_gauge") == 0) {
            cJSON *valj = cJSON_GetObjectItem(item, "value");
            cJSON *typej = cJSON_GetObjectItem(item, "type");
            cJSON *unitj = cJSON_GetObjectItem(item, "unit");
            assert(cJSON_IsNumber(valj) && fabs(valj->valuedouble - 3.14) < 1e-9);
            assert(cJSON_IsString(typej) && strcmp(typej->valuestring, "gauge") == 0);
            assert(cJSON_IsString(unitj) && strcmp(unitj->valuestring, "bytes") == 0);
            found = 1;
        }
    }
    assert(found);
    cJSON_Delete(root);
    AIRY_FREE(resp);

    /* 带 name：按名称过滤 */
    resp = rpc_roundtrip(&svc, "query_metrics", "{\"name\":\"rpc_gauge\"}", 10);
    assert(resp != NULL);
    root = rpc_expect_success(resp, 10);
    result = cJSON_GetObjectItem(root, "result");
    count = cJSON_GetObjectItem(result, "count");
    assert(cJSON_IsNumber(count) && count->valueint == 1);
    cJSON_Delete(root);
    AIRY_FREE(resp);

    test_svc_teardown(&svc);
    printf("    PASSED\n");
}

static void test_jsonrpc_get_metrics_alias(void)
{
    printf("  test_jsonrpc_get_metrics_alias...\n");
    observe_d_service_t svc = {0};
    test_svc_setup(&svc);

    /* get_metrics 为 query_metrics 的别名，行为一致 */
    char *resp = rpc_roundtrip(&svc, "get_metrics", NULL, 11);
    assert(resp != NULL);
    cJSON *root = rpc_expect_success(resp, 11);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *count = cJSON_GetObjectItem(result, "count");
    assert(cJSON_IsNumber(count) && count->valueint == (double)svc.metric_count);
    cJSON_Delete(root);
    AIRY_FREE(resp);

    test_svc_teardown(&svc);
    printf("    PASSED\n");
}
#endif /* !_WIN32 */

int main(void)
{
    printf("=========================================\n");
    printf("  Observe Service Unit Tests\n");
    printf("=========================================\n");
    fflush(stdout);

    airy_log_init(NULL); /* SVC_LOG_INFO 依赖日志系统（同 observe_d main） */

    test_record_metric_queryable();
    test_counter_accumulate();
    test_gauge_overwrite();
#ifndef _WIN32
    test_jsonrpc_record_metric();
    test_jsonrpc_record_metric_invalid();
    test_jsonrpc_query_metrics();
    test_jsonrpc_get_metrics_alias();
#endif

    printf("\nAll observe service tests PASSED\n");
    return 0;
}
