// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 *
 * @file test_gateway_jsonpick.c
 * @brief 网关 JSON 字段提取机制单测
 *
 * 验证 gw_json_* 家族（cJSON 唯一实现）的取值语义与失败回退：
 *   - load：正常 / NULL / 非法 JSON
 *   - str：顶层 string / 缺失 / 非 string
 *   - raw：string 带引号、object、array、number 原文 / bool / 缺失
 *   - num/int：命中、缺省回退、类型不符回退
 *   - 键名误匹配回归：字符串值内嵌键文本不得遮蔽顶层同名字段
 *     （手写 strstr 实现的历史缺陷，收敛后必须保持正确）
 */

#include "gateway_jsonpick.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airy_memory.h"

static void test_load(void)
{
    cJSON *root = gw_json_load("{\"a\":1}");
    assert(root);
    cJSON_Delete(root);

    assert(gw_json_load(NULL) == NULL);
    assert(gw_json_load("{not json") == NULL);
    printf("  test_load ok\n");
}

static void test_str(void)
{
    cJSON *root = gw_json_load("{\"model\":\"gpt-x\",\"n\":5}");
    assert(root);
    char *s = gw_json_str(root, "model");
    assert(s && strcmp(s, "gpt-x") == 0);
    AIRY_FREE(s);

    assert(gw_json_str(root, "missing") == NULL);
    assert(gw_json_str(root, "n") == NULL); /* 非 string */
    cJSON_Delete(root);
    printf("  test_str ok\n");
}

static void test_raw(void)
{
    cJSON *root = gw_json_load(
        "{\"s\":\"val\",\"o\":{\"k\":1},\"a\":[1,2],\"num\":42,\"t\":true,\"z\":null}");
    assert(root);

    char *s = gw_json_raw(root, "s");
    assert(s && strcmp(s, "\"val\"") == 0);
    AIRY_FREE(s);

    char *o = gw_json_raw(root, "o");
    assert(o && strcmp(o, "{\"k\":1}") == 0);
    AIRY_FREE(o);

    char *a = gw_json_raw(root, "a");
    assert(a && strcmp(a, "[1,2]") == 0);
    AIRY_FREE(a);

    char *n = gw_json_raw(root, "num");
    assert(n && strcmp(n, "42") == 0);
    AIRY_FREE(n);

    assert(gw_json_raw(root, "t") == NULL); /* bool 不回显 */
    assert(gw_json_raw(root, "z") == NULL);
    assert(gw_json_raw(root, "missing") == NULL);
    cJSON_Delete(root);
    printf("  test_raw ok\n");
}

static void test_numbers(void)
{
    cJSON *root = gw_json_load("{\"temp\":0.7,\"tok\":512,\"s\":\"x\"}");
    assert(root);

    assert(gw_json_num(root, "temp", 9.5) == 0.7);
    assert(gw_json_num(root, "missing", 9.5) == 9.5);
    assert(gw_json_num(root, "tok", 9.5) == 512.0);
    assert(gw_json_num(root, "s", 9.5) == 9.5); /* 非 number 字段回退 */

    assert(gw_json_int(root, "tok", -1) == 512);
    assert(gw_json_int(root, "missing", -1) == -1);
    assert(gw_json_int(root, "s", -1) == -1); /* 非 number 字段回退 */
    cJSON_Delete(root);
    printf("  test_numbers ok\n");
}

static void test_no_key_shadowing(void)
{
    /* 回归：value 内含键名文本时，手写 strstr 曾误匹配此处 */
    cJSON *root = gw_json_load("{\"name\":\"say \\\"method\\\" ok\",\"method\":\"tools/list\"}");
    assert(root);
    char *m = gw_json_str(root, "method");
    assert(m && strcmp(m, "tools/list") == 0);
    AIRY_FREE(m);

    /* 嵌套同名字段不得遮蔽顶层 */
    cJSON *nested = gw_json_load("{\"params\":{\"id\":\"inner\"},\"id\":\"outer\"}");
    assert(nested);
    char *id = gw_json_str(nested, "id");
    assert(id && strcmp(id, "outer") == 0);
    AIRY_FREE(id);
    cJSON_Delete(nested);
    cJSON_Delete(root);
    printf("  test_no_key_shadowing ok\n");
}

int main(void)
{
    printf("test_gateway_jsonpick:\n");
    test_load();
    test_str();
    test_raw();
    test_numbers();
    test_no_key_shadowing();
    printf("all gateway_jsonpick tests passed\n");
    return 0;
}
