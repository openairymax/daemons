// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_jsonrpc_helpers.c
 * @brief JSON-RPC 辅助函数单元测试
 */

#include "jsonrpc_helpers.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PASS(name) printf("✓ %s\n", name)
#define TEST_FAIL(name, reason) printf("✗ %s: %s\n", name, reason)

static int test_build_error_response(void)
{
    char *resp = jsonrpc_build_error(JSONRPC_INVALID_PARAMS, "Missing field", 1);
    if (!resp) {
        TEST_FAIL("build_error_response", "NULL response");
        return -1;
    }

    if (strstr(resp, "\"jsonrpc\":\"2.0\"") == NULL) {
        TEST_FAIL("build_error_response", "missing jsonrpc version");
        free(resp);
        return -1;
    }

    if (strstr(resp, "\"id\":1") == NULL) {
        TEST_FAIL("build_error_response", "missing id");
        free(resp);
        return -1;
    }

    char expected_code[32];
    snprintf(expected_code, sizeof(expected_code), "\"code\":%d", JSONRPC_INVALID_PARAMS);
    if (strstr(resp, expected_code) == NULL) {
        TEST_FAIL("build_error_response", "missing error code");
        free(resp);
        return -1;
    }

    if (strstr(resp, "\"message\":\"Missing field\"") == NULL) {
        TEST_FAIL("build_error_response", "missing error message");
        free(resp);
        return -1;
    }

    free(resp);
    TEST_PASS("build_error_response");
    return 0;
}

static int test_build_success_response(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "ok");

    char *resp = jsonrpc_build_success(result, 2);
    if (!resp) {
        TEST_FAIL("build_success_response", "NULL response");
        return -1;
    }

    if (strstr(resp, "\"jsonrpc\":\"2.0\"") == NULL) {
        TEST_FAIL("build_success_response", "missing jsonrpc version");
        free(resp);
        return -1;
    }

    if (strstr(resp, "\"id\":2") == NULL) {
        TEST_FAIL("build_success_response", "missing id");
        free(resp);
        return -1;
    }

    if (strstr(resp, "\"status\":\"ok\"") == NULL) {
        TEST_FAIL("build_success_response", "missing result");
        free(resp);
        return -1;
    }

    free(resp);
    TEST_PASS("build_success_response");
    return 0;
}

static int test_parse_request(void)
{
    const char *raw =
        "{\"jsonrpc\":\"2.0\",\"method\":\"test\",\"params\":{\"key\":\"value\"},\"id\":42}";
    cJSON *req = cJSON_Parse(raw);
    if (!req) {
        TEST_FAIL("parse_request", "cJSON_Parse failed");
        return -1;
    }

    char *method = NULL;
    cJSON *params = NULL;
    int id = 0;

    if (jsonrpc_parse_request_ptr(req, &method, &params, &id) != 0) {
        TEST_FAIL("parse_request", "parse failed");
        goto out_fail;
    }

    if (strcmp(method, "test") != 0) {
        TEST_FAIL("parse_request", "wrong method");
        goto out_fail;
    }

    if (id != 42) {
        TEST_FAIL("parse_request", "wrong id");
        goto out_fail;
    }

    if (!params) {
        TEST_FAIL("parse_request", "missing params");
        goto out_fail;
    }

    cJSON *key = cJSON_GetObjectItem(params, "key");
    if (!key || strcmp(key->valuestring, "value") != 0) {
        TEST_FAIL("parse_request", "wrong params");
        goto out_fail;
    }

    free(method);
    cJSON_Delete(params);
    cJSON_Delete(req);

    TEST_PASS("parse_request");
    return 0;

out_fail:
    free(method);
    if (params)
        cJSON_Delete(params);
    cJSON_Delete(req);
    return -1;
}

static int test_parse_invalid_request(void)
{
    const char *raw = "{\"jsonrpc\":\"1.0\",\"method\":\"test\"}";
    cJSON *req = cJSON_Parse(raw);
    if (!req) {
        TEST_FAIL("parse_invalid_request", "cJSON_Parse failed");
        return -1;
    }

    char *method = NULL;
    cJSON *params = NULL;
    int id = 0;

    if (jsonrpc_parse_request_ptr(req, &method, &params, &id) == 0) {
        TEST_FAIL("parse_invalid_request", "should fail for invalid version");
        free(method);
        if (params)
            cJSON_Delete(params);
        cJSON_Delete(req);
        return -1;
    }

    cJSON_Delete(req);
    TEST_PASS("parse_invalid_request");
    return 0;
}

static int test_get_string_param(void)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", "test_value");

    const char *value = jsonrpc_get_string_param(params, "name", "default");
    if (strcmp(value, "test_value") != 0) {
        TEST_FAIL("get_string_param", "wrong value");
        cJSON_Delete(params);
        return -1;
    }

    const char *missing = jsonrpc_get_string_param(params, "missing", "default");
    if (strcmp(missing, "default") != 0) {
        TEST_FAIL("get_string_param", "wrong default");
        cJSON_Delete(params);
        return -1;
    }

    cJSON_Delete(params);
    TEST_PASS("get_string_param");
    return 0;
}

static int test_build_notification(void)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "status", "updated");

    char *notif = jsonrpc_build_notification("update", params);
    if (!notif) {
        TEST_FAIL("build_notification", "NULL notification");
        return -1;
    }

    if (strstr(notif, "\"method\":\"update\"") == NULL) {
        TEST_FAIL("build_notification", "missing method");
        free(notif);
        return -1;
    }

    if (strstr(notif, "\"id\"") != NULL) {
        TEST_FAIL("build_notification", "should not have id");
        free(notif);
        return -1;
    }

    free(notif);
    TEST_PASS("build_notification");
    return 0;
}

static int test_get_error_message(void)
{
    const char *msg = jsonrpc_get_error_message(JSONRPC_PARSE_ERROR);
    if (strcmp(msg, "Parse error") != 0) {
        TEST_FAIL("get_error_message", "wrong message");
        return -1;
    }

    msg = jsonrpc_get_error_message(JSONRPC_INVALID_PARAMS);
    if (strcmp(msg, "Invalid params") != 0) {
        TEST_FAIL("get_error_message", "wrong message");
        return -1;
    }

    msg = jsonrpc_get_error_message(-32050);
    if (strcmp(msg, "Unknown error") != 0) {
        TEST_FAIL("get_error_message", "wrong server error message");
        return -1;
    }

    msg = jsonrpc_get_error_message(999);
    if (strcmp(msg, "Unknown error") != 0) {
        TEST_FAIL("get_error_message", "wrong unknown error message");
        return -1;
    }

    TEST_PASS("get_error_message");
    return 0;
}

int main(void)
{
    int failed = 0;

    printf("\n=== JSON-RPC Helpers Unit Tests ===\n\n");

    if (test_build_error_response() != 0)
        failed++;
    if (test_build_success_response() != 0)
        failed++;
    if (test_parse_request() != 0)
        failed++;
    if (test_parse_invalid_request() != 0)
        failed++;
    if (test_get_string_param() != 0)
        failed++;
    if (test_build_notification() != 0)
        failed++;
    if (test_get_error_message() != 0)
        failed++;

    printf("\n=== Test Summary ===\n");
    printf("Total: 7 tests\n");
    printf("Passed: %d\n", 7 - failed);
    printf("Failed: %d\n", failed);

    return (failed == 0) ? 0 : 1;
}
