// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_cost_tracker.c
 * @brief 成本跟踪器单元测试
 */

#include "cost_tracker.h"
#include "platform.h"

#include <assert.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 测试成本跟踪器创建和销毁
 */
static void test_cost_tracker_create_destroy(void)
{
    printf("  test_cost_tracker_create_destroy...\n");

    cost_tracker_t *tracker = cost_tracker_create(NULL, 0);
    assert(tracker != NULL);

    cost_tracker_destroy(tracker);

    printf("    PASSED\n");
}

/**
 * @brief 测试成本跟踪器带规则创建
 */
static void test_cost_tracker_with_rules(void)
{
    printf("  test_cost_tracker_with_rules...\n");

    pricing_rule_t rules[2];
    memset(rules, 0, sizeof(rules));

    rules[0].model_pattern = strdup("gpt-4*");
    rules[0].input_price_per_k = 0.03;
    rules[0].output_price_per_k = 0.06;

    rules[1].model_pattern = strdup("gpt-3.5-turbo");
    rules[1].input_price_per_k = 0.001;
    rules[1].output_price_per_k = 0.002;

    cost_tracker_t *tracker = cost_tracker_create(rules, 2);
    assert(tracker != NULL);

    cost_tracker_destroy(tracker);

    free((void *)rules[0].model_pattern);
    free((void *)rules[1].model_pattern);

    printf("    PASSED\n");
}

/**
 * @brief 测试成本跟踪器添加记录
 */
static void test_cost_tracker_add(void)
{
    printf("  test_cost_tracker_add...\n");

    cost_tracker_t *tracker = cost_tracker_create(NULL, 0);
    assert(tracker != NULL);

    cost_tracker_add(tracker, "gpt-4", 1000, 500, 0);
    cost_tracker_add(tracker, "gpt-4", 2000, 1000, 250);
    cost_tracker_add(tracker, "gpt-3.5-turbo", 500, 250, 0);

    cost_tracker_destroy(tracker);

    printf("    PASSED\n");
}

/**
 * @brief 测试成本跟踪器导出JSON
 */
static void test_cost_tracker_export(void)
{
    printf("  test_cost_tracker_export...\n");

    pricing_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.model_pattern = strdup("gpt-4");
    rule.input_price_per_k = 0.03;
    rule.output_price_per_k = 0.06;

    cost_tracker_t *tracker = cost_tracker_create(&rule, 1);
    assert(tracker != NULL);

    cost_tracker_add(tracker, "gpt-4", 1000, 500, 300);
    cost_tracker_add(tracker, "gpt-4", 1000, 500, 200);

    cJSON *json = cost_tracker_export(tracker);
    assert(json != NULL);

    cJSON *models = cJSON_GetObjectItem(json, "models");
    assert(models != NULL);
    assert(cJSON_IsArray(models));

    int count = cJSON_GetArraySize(models);
    assert(count == 1);
    (void)count;

    cJSON *model_obj = cJSON_GetArrayItem(models, 0);
    cJSON *model_name = cJSON_GetObjectItem(model_obj, "model");
    assert(model_name != NULL);
    (void)model_name;
    assert(strcmp(model_name->valuestring, "gpt-4") == 0);

    cJSON *cached = cJSON_GetObjectItem(model_obj, "cached_tokens");
    assert(cJSON_IsNumber(cached) && cached->valuedouble == 500);

    cJSON_Delete(json);
    cost_tracker_destroy(tracker);
    free((void *)rule.model_pattern);

    printf("    PASSED\n");
}

/**
 * @brief 测试成本跟踪器空参数
 */
static void test_cost_tracker_null_param(void)
{
    printf("  test_cost_tracker_null_param...\n");

    cost_tracker_add(NULL, "gpt-4", 1000, 500, 0);

    cost_tracker_t *tracker = cost_tracker_create(NULL, 0);
    cost_tracker_add(tracker, NULL, 1000, 500, 0);

    cJSON *json = cost_tracker_export(NULL);
    assert(json != NULL);
    cJSON_Delete(json);

    cost_tracker_destroy(tracker);
    cost_tracker_destroy(NULL);

    printf("    PASSED\n");
}

/**
 * @brief 测试成本跟踪器多模型
 */
static void test_cost_tracker_multiple_models(void)
{
    printf("  test_cost_tracker_multiple_models...\n");

    cost_tracker_t *tracker = cost_tracker_create(NULL, 0);
    assert(tracker != NULL);

    cost_tracker_add(tracker, "gpt-4", 1000, 500, 0);
    cost_tracker_add(tracker, "gpt-3.5-turbo", 2000, 1000, 0);
    cost_tracker_add(tracker, "claude-3", 1500, 750, 0);

    cJSON *json = cost_tracker_export(tracker);
    assert(json != NULL);

    cJSON *models = cJSON_GetObjectItem(json, "models");
    int count = cJSON_GetArraySize(models);
    assert(count == 3);
    (void)count;

    cJSON_Delete(json);
    cost_tracker_destroy(tracker);

    printf("    PASSED\n");
}

/**
 * @brief 测试成本跟踪器规则匹配
 */
static void test_cost_tracker_rule_matching(void)
{
    printf("  test_cost_tracker_rule_matching...\n");

    pricing_rule_t rules[2];
    memset(rules, 0, sizeof(rules));

    rules[0].model_pattern = strdup("gpt-4*");
    rules[0].input_price_per_k = 0.03;
    rules[0].output_price_per_k = 0.06;

    rules[1].model_pattern = strdup("gpt-3.5*");
    rules[1].input_price_per_k = 0.001;
    rules[1].output_price_per_k = 0.002;

    cost_tracker_t *tracker = cost_tracker_create(rules, 2);
    assert(tracker != NULL);

    cost_tracker_add(tracker, "gpt-4-turbo", 1000, 500, 0);
    cost_tracker_add(tracker, "gpt-3.5-turbo-16k", 1000, 500, 0);

    cJSON *json = cost_tracker_export(tracker);
    assert(json != NULL);

    cJSON *models = cJSON_GetObjectItem(json, "models");
    int count = cJSON_GetArraySize(models);
    assert(count == 2);
    (void)count;

    cJSON_Delete(json);
    cost_tracker_destroy(tracker);

    free((void *)rules[0].model_pattern);
    free((void *)rules[1].model_pattern);

    printf("    PASSED\n");
}

/* 落盘窗口回归（0.1.20 P1-1）：窗口内零 I/O，达次数阈值或间隔到期才写，
 * 退出路径仍无条件同步落盘。观测口径为文件是否存在及其 prompt_tokens 累计。 */
static const char *tmp_base(void)
{
#if defined(_WIN32)
    const char *t = getenv("TEMP");
    return (t && t[0]) ? t : ".";
#else
    const char *t = getenv("TMPDIR");
    return (t && t[0]) ? t : "/tmp";
#endif
}

static long file_prompt_tokens(const char *path)
{
    FILE *f = fopen(path, "rb");
    char buf[4096];
    size_t n;
    long value = -1;

    if (!f)
        return -1;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root) {
        cJSON *models = cJSON_GetObjectItem(root, "models");
        cJSON *first = cJSON_IsArray(models) ? cJSON_GetArrayItem(models, 0) : NULL;
        cJSON *pt = cJSON_IsObject(first) ? cJSON_GetObjectItem(first, "prompt_tokens") : NULL;
        if (cJSON_IsNumber(pt))
            value = (long)pt->valuedouble;
        cJSON_Delete(root);
    }
    return value;
}

static void test_cost_tracker_flush_window(void)
{
    printf("  test_cost_tracker_flush_window...\n");

    char path[512];
    snprintf(path, sizeof(path), "%s/airy_cost_flush_test.json", tmp_base());
    (void)remove(path);

    cost_tracker_t *tracker = cost_tracker_create(NULL, 0);
    assert(tracker != NULL);
    assert(cost_tracker_flush(NULL, path) == -1);

    /* 无新增：dirty 为假，即便窗口已过也不产生文件。 */
    assert(cost_tracker_flush(tracker, path) == 0);
    assert(file_prompt_tokens(path) == -1);

    /* 单次累计在窗口内：不落盘。 */
    cost_tracker_add(tracker, "gpt-4", 100, 50, 0);
    assert(cost_tracker_flush(tracker, path) == 0);
    assert(file_prompt_tokens(path) == -1);

    /* 累计到 64 次：达次数阈值，立即落盘。 */
    for (int i = 1; i < 64; ++i)
        cost_tracker_add(tracker, "gpt-4", 100, 50, 0);
    assert(cost_tracker_flush(tracker, path) == 0);
    assert(file_prompt_tokens(path) == 6400);

    /* 落盘后窗口重开：窗口内的新增不写盘，文件内容保持不变。 */
    cost_tracker_add(tracker, "gpt-4", 100, 50, 0);
    assert(cost_tracker_flush(tracker, path) == 0);
    assert(file_prompt_tokens(path) == 6400);

    /* 间隔阈值：窗口到期后把积压的累计一次写掉。 */
    airy_sleep_ms(1100);
    assert(cost_tracker_flush(tracker, path) == 0);
    assert(file_prompt_tokens(path) == 6500);

    /* 退出路径：save 无条件同步落盘，不受窗口约束。 */
    cost_tracker_add(tracker, "gpt-4", 10, 5, 0);
    assert(cost_tracker_save(tracker, path) == 0);
    assert(file_prompt_tokens(path) == 6510);

    /* 稳态 1000 请求：请求路径不出现同步写，只有次数触发的页缓存刷新。 */
    for (int i = 0; i < 1000; ++i) {
        cost_tracker_add(tracker, "gpt-4", 100, 50, 0);
        assert(cost_tracker_flush(tracker, path) == 0);
    }
    assert(file_prompt_tokens(path) >= 6510);
    assert(cost_tracker_save(tracker, path) == 0);
    assert(file_prompt_tokens(path) == 106510);

    cost_tracker_destroy(tracker);
    (void)remove(path);

    printf("    PASSED\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("  Cost Tracker Unit Tests\n");
    printf("=========================================\n");

    test_cost_tracker_create_destroy();
    test_cost_tracker_with_rules();
    test_cost_tracker_add();
    test_cost_tracker_export();
    test_cost_tracker_null_param();
    test_cost_tracker_multiple_models();
    test_cost_tracker_rule_matching();
    test_cost_tracker_flush_window();

    printf("\n✅ All cost tracker tests PASSED\n");
    return 0;
}
