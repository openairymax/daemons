// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_invoke_payload.c
 * @brief runner 失败载荷契约直测（white-box include，无 daemon 依赖）。
 *
 * 锁死 S-P0-09：runner.py 回报 success:false 时 inv_take 必须返回失败，
 * error 文本原样留在 out_output 供上层回传。此前该路径把错误 JSON 当成功
 * 输出上交，网关与调度层据此认为任务已完成，用户看到一段"正常回答"。
 */

#include "../src/service/service_invoke.c"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s\n", msg);                                                             \
            g_fail++;                                                                              \
        }                                                                                          \
    } while (0)

static void test_failure_is_failure(void)
{
    const char *raw = "{\"success\":false,\"error\":\"model auth failed\"}";
    cJSON *resp = cJSON_Parse(raw);
    CHECK(resp != NULL, "failure response must parse");
    if (!resp)
        return;
    char *out = NULL;
    CHECK(inv_take(resp, raw, &out) == -1, "success:false must be a failure");
    cJSON *obj = out ? cJSON_Parse(out) : NULL;
    CHECK(obj != NULL, "failure payload must stay JSON");
    if (obj) {
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(obj, "success")), "payload keeps success=false");
        cJSON *err = cJSON_GetObjectItem(obj, "error");
        CHECK(err && cJSON_IsString(err), "payload keeps error text");
        if (err && cJSON_IsString(err))
            CHECK(strcmp(err->valuestring, "model auth failed") == 0, "error text verbatim");
    }
    cJSON_Delete(obj);
    AIRY_FREE(out);
    cJSON_Delete(resp);
}

static void test_failure_without_text_still_fails(void)
{
    const char *raw = "{\"success\":false}";
    cJSON *resp = cJSON_Parse(raw);
    CHECK(resp != NULL, "bare failure response must parse");
    if (!resp)
        return;
    char *out = NULL;
    CHECK(inv_take(resp, raw, &out) == -1, "missing error field must not become success");
    CHECK(out != NULL && strstr(out, "\"success\":false") != NULL, "generic failure payload");
    AIRY_FREE(out);
    cJSON_Delete(resp);
}

static void test_success_passes_output(void)
{
    const char *raw = "{\"success\":true,\"output\":\"hello\"}";
    cJSON *resp = cJSON_Parse(raw);
    CHECK(resp != NULL, "success response must parse");
    if (!resp)
        return;
    char *out = NULL;
    CHECK(inv_take(resp, raw, &out) == 0, "success:true must pass");
    CHECK(out && strcmp(out, "hello") == 0, "output text is relayed verbatim");
    AIRY_FREE(out);
    cJSON_Delete(resp);
}

static void test_success_without_output_relays_raw(void)
{
    const char *raw = "{\"success\":true}";
    cJSON *resp = cJSON_Parse(raw);
    CHECK(resp != NULL, "output-less response must parse");
    if (!resp)
        return;
    char *out = NULL;
    CHECK(inv_take(resp, raw, &out) == 0, "success without output must pass");
    CHECK(out && strcmp(out, raw) == 0, "raw response is relayed");
    AIRY_FREE(out);
    cJSON_Delete(resp);
}

int main(void)
{
    test_failure_is_failure();
    test_failure_without_text_still_fails();
    test_success_passes_output();
    test_success_without_output_relays_raw();
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_invoke_payload: all checks passed\n");
    return 0;
}
