// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_observe_rpc.c
 * @brief observe 域（monit_d 内建模块）单元测试（0.1.9 M4：observe_d → monit_d）。
 *
 * 直接编译 observe_rpc.c 真实实现（禁止桩函数），覆盖动态指标表
 * （record/counter 累加/gauge 覆盖/文本导出）与调度器 wire 路径
 * （observe_record_metric / observe_query_metrics / observe_get_metrics）。
 */

#include "airy_memory.h"
#include "jsonrpc_helpers.h"
#include "method_dispatcher.h"
#include "observe_rpc.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

static method_dispatcher_t *g_disp;

static void test_record_and_format(void)
{
    printf("  test_record_and_format...\n");
    assert(obs_rpc_record("test_latency_ms", 12.5, "ms", OBS_GAUGE) == 0);

    static char buf[65536];
    int len = obs_rpc_format(buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "# TYPE test_latency_ms gauge") != NULL);
    assert(strstr(buf, "test_latency_ms 12.500000") != NULL);
    printf("    PASSED\n");
}

static void test_counter_accumulate(void)
{
    printf("  test_counter_accumulate...\n");
    assert(obs_rpc_record("test_counter", 1.0, "count", OBS_COUNTER) == 0);
    assert(obs_rpc_record("test_counter", 2.0, "count", OBS_COUNTER) == 0);
    assert(obs_rpc_record("test_counter", 3.5, "count", OBS_COUNTER) == 0);

    static char buf[65536];
    assert(obs_rpc_format(buf, sizeof(buf)) > 0);
    assert(strstr(buf, "test_counter 6.500000") != NULL);
    assert(strstr(buf, "# TYPE test_counter counter") != NULL);
    printf("    PASSED\n");
}

static void test_gauge_overwrite(void)
{
    printf("  test_gauge_overwrite...\n");
    assert(obs_rpc_record("test_gauge", 1.0, NULL, OBS_GAUGE) == 0);
    assert(obs_rpc_record("test_gauge", 42.0, NULL, OBS_GAUGE) == 0);

    static char buf[65536];
    assert(obs_rpc_format(buf, sizeof(buf)) > 0);
    assert(strstr(buf, "test_gauge 42.000000") != NULL);
    assert(strstr(buf, "test_gauge 1.000000") == NULL);
    printf("    PASSED\n");
}

static void test_null_name_rejected(void)
{
    printf("  test_null_name_rejected...\n");
    assert(obs_rpc_record(NULL, 1.0, NULL, OBS_GAUGE) != 0);
    printf("    PASSED\n");
}

#ifndef _WIN32

static char *rpc_roundtrip(const char *method, const char *params_json, int id)
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

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    assert(method_dispatcher_dispatch(g_disp, root, jsonrpc_build_error, &fds[0]) == 0);
    cJSON_Delete(root);

    /* 响应可能超过单次 read：循环读直到拼出完整 JSON */
    size_t cap = 32768, len = 0;
    char *buf = AIRY_MALLOC(cap);
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            buf = AIRY_REALLOC(buf, cap);
            assert(buf != NULL);
        }
        ssize_t r = read(fds[1], buf + len, cap - len - 1);
        assert(r > 0);
        len += (size_t)r;
        buf[len] = '\0';
        cJSON *probe = cJSON_Parse(buf);
        if (probe) {
            cJSON_Delete(probe);
            break;
        }
    }
    close(fds[0]);
    close(fds[1]);
    return buf;
}

static cJSON *rpc_expect_success(const char *resp, int id)
{
    cJSON *root = cJSON_Parse(resp);
    assert(root != NULL);
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
    char *resp = rpc_roundtrip("observe_record_metric",
                               "{\"name\":\"rpc_counter\",\"value\":5.0,"
                               "\"type\":\"counter\",\"unit\":\"count\"}",
                               7);
    assert(resp != NULL);
    cJSON *root = rpc_expect_success(resp, 7);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *status = cJSON_GetObjectItem(result, "status");
    assert(cJSON_IsString(status) && strcmp(status->valuestring, "recorded") == 0);
    assert(strstr(resp, "\"name\":\"rpc_counter\"") != NULL); /* 响应回显 name 字段 */
    cJSON_Delete(root);
    AIRY_FREE(resp);

    static char buf[65536];
    assert(obs_rpc_format(buf, sizeof(buf)) > 0);
    assert(strstr(buf, "rpc_counter 5.000000") != NULL);
    printf("    PASSED\n");
}

static void test_jsonrpc_record_metric_invalid(void)
{
    printf("  test_jsonrpc_record_metric_invalid...\n");
    char *resp = rpc_roundtrip("observe_record_metric", "{\"value\":1.0}", 8);
    assert(resp != NULL);
    cJSON *root = cJSON_Parse(resp);
    assert(root != NULL);
    cJSON *err = cJSON_GetObjectItem(root, "error");
    assert(cJSON_IsObject(err));
    cJSON *code = cJSON_GetObjectItem(err, "code");
    assert(cJSON_IsNumber(code) && code->valueint == JSONRPC_INVALID_PARAMS);
    cJSON_Delete(root);
    AIRY_FREE(resp);
    printf("    PASSED\n");
}

static void test_jsonrpc_query_metrics(void)
{
    printf("  test_jsonrpc_query_metrics...\n");
    assert(obs_rpc_record("rpc_gauge", 3.14, "bytes", OBS_GAUGE) == 0);

    char *resp = rpc_roundtrip("observe_query_metrics", NULL, 9);
    assert(resp != NULL);
    cJSON *root = rpc_expect_success(resp, 9);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *count = cJSON_GetObjectItem(result, "count");
    assert(cJSON_IsNumber(count) &&
           (size_t)count->valueint == obs_rpc_metric_count());
    cJSON *arr = cJSON_GetObjectItem(result, "metrics");
    assert(cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == (int)count->valueint);
    int found = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        cJSON *namej = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(namej) && strcmp(namej->valuestring, "rpc_gauge") == 0) {
            cJSON *valj = cJSON_GetObjectItem(item, "value");
            cJSON *typej = cJSON_GetObjectItem(item, "type");
            cJSON *unitj = cJSON_GetObjectItem(item, "unit");
            assert(cJSON_IsNumber(valj) && fabs(valj->valuedouble - 3.14) < 1e-6);
            assert(cJSON_IsString(typej) && strcmp(typej->valuestring, "gauge") == 0);
            assert(cJSON_IsString(unitj) && strcmp(unitj->valuestring, "bytes") == 0);
            found = 1;
        }
    }
    assert(found);
    cJSON_Delete(root);
    AIRY_FREE(resp);

    resp = rpc_roundtrip("observe_query_metrics", "{\"name\":\"rpc_gauge\"}", 10);
    assert(resp != NULL);
    root = rpc_expect_success(resp, 10);
    result = cJSON_GetObjectItem(root, "result");
    count = cJSON_GetObjectItem(result, "count");
    assert(cJSON_IsNumber(count) && count->valueint == 1);
    cJSON_Delete(root);
    AIRY_FREE(resp);
    printf("    PASSED\n");
}

static void test_jsonrpc_get_metrics_alias(void)
{
    printf("  test_jsonrpc_get_metrics_alias...\n");
    char *resp = rpc_roundtrip("observe_get_metrics", NULL, 11);
    assert(resp != NULL);
    cJSON *root = rpc_expect_success(resp, 11);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *count = cJSON_GetObjectItem(result, "count");
    assert(cJSON_IsNumber(count) &&
           (size_t)count->valueint == obs_rpc_metric_count());
    cJSON_Delete(root);
    AIRY_FREE(resp);
    printf("    PASSED\n");
}

static void test_metrics_http_fused(void)
{
    printf("  test_metrics_http_fused...\n");
    const char *req = "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n";
    char *response = NULL;
    size_t response_len = 0;
    assert(obs_rpc_handle_http(req, strlen(req), &response, &response_len) == 0);
    assert(response != NULL && response_len > 0);
    assert(strstr(response, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(response, "Content-Length:") != NULL);
    /* 动态表条目必须出现在融合响应中 */
    assert(strstr(response, "test_latency_ms") != NULL);
    AIRY_FREE(response);

    /* 非 /metrics 请求不误接管（交给 monit 其他通路） */
    const char *other = "GET /other HTTP/1.1\r\n\r\n";
    assert(obs_rpc_handle_http(other, strlen(other), &response, &response_len) == -1);
    printf("    PASSED\n");
}
#endif /* !_WIN32 */

int main(void)
{
    printf("=========================================\n");
    printf("  Monit observe_rpc Module Tests\n");
    printf("=========================================\n");
    fflush(stdout);

    airy_log_init(NULL);
    assert(observe_rpc_init() == 0);
#ifndef _WIN32
    g_disp = method_dispatcher_create(16);
    assert(g_disp != NULL);
    observe_rpc_register(g_disp);
    /* 自监控预注册 5 个指标 + 非 /metrics 请求不误接管 */
    assert(obs_rpc_metric_count() >= 5);
#endif

    test_record_and_format();
    test_counter_accumulate();
    test_gauge_overwrite();
    test_null_name_rejected();
#ifndef _WIN32
    test_jsonrpc_record_metric();
    test_jsonrpc_record_metric_invalid();
    test_jsonrpc_query_metrics();
    test_jsonrpc_get_metrics_alias();
    test_metrics_http_fused();
    method_dispatcher_destroy(g_disp);
#endif

    observe_rpc_cleanup();
    printf("\nAll observe_rpc tests PASSED\n");
    return 0;
}
