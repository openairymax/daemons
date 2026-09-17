// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_gen_params.c
 * @brief C-4 (R3) 生成参数唯一解析点回归测试
 *
 * 锁定 resolve_gen_params() 的三方优先级契约，防止同步/流式双路径口径漂移：
 *
 *   1. 显式意图在边界内            → 取意图
 *   2. 显式意图超出模型声明上限    → 按上限截断（配置写了上限必须兑现）
 *   3. 未声明意图、模型有上限      → 取模型上限（优先于引擎默认）
 *   4. 未声明意图、模型无上限      → 取引擎默认上限
 *   5. 三者皆无                    → 0（不写 max_tokens，交上游默认）
 *   6. 负值意图                    → 归零，不得下传负值
 *   7. manager->model 缺省         → 落 svc->default_model
 *   8. provider 为 NULL            → 无模型上限，落引擎默认
 *
 * 同时覆盖 provider_registry_model_max_output() 的边界（NULL / 未命中 / 命中）。
 */

#include "llm_service_internal.h"
#include "service.h"

#include <stdio.h>
#include <string.h>

static int test_count = 0;
static int fail_count = 0;

static void check_int(const char *name, int got, int want)
{
    test_count++;
    if (got != want) {
        printf("    FAIL: %s: got %d, want %d\n", name, got, want);
        fail_count++;
    }
}

static void check_str(const char *name, const char *got, const char *want)
{
    test_count++;
    if (strcmp(got, want) != 0) {
        printf("    FAIL: %s: got \"%s\", want \"%s\"\n", name, got, want);
        fail_count++;
    }
}

static void make_provider(provider_t *prov, char **models, int *caps)
{
    __builtin_memset(prov, 0, sizeof(*prov));
    prov->name = "test-prov";
    prov->models = models;
    prov->model_max_output = caps;
}

static void run_resolve(const llm_service_t *svc, const provider_t *prov, const char *req_model,
                        int req_max, char *out_model, size_t model_size, int *out_max)
{
    llm_request_config_t cfg;
    __builtin_memset(&cfg, 0, sizeof(cfg));
    cfg.model = req_model;
    cfg.max_tokens = req_max;
    resolve_gen_params(svc, prov, &cfg, out_model, model_size, out_max);
}

static void test_priority_within_cap(void)
{
    llm_service_t svc;
    __builtin_memset(&svc, 0, sizeof(svc));
    strcpy(svc.default_model, "engine-default");
    svc.default_max_output_tokens = 8000;

    char *models[] = {"m1", NULL};
    int caps[] = {16000, 0};
    provider_t prov;
    make_provider(&prov, models, caps);

    char model[128];
    int max_tokens = -1;
    run_resolve(&svc, &prov, "m1", 1000, model, sizeof(model), &max_tokens);
    check_int("within_cap.max_tokens", max_tokens, 1000);
    check_str("within_cap.model", model, "m1");
}

static void test_explicit_clipped_by_cap(void)
{
    llm_service_t svc;
    __builtin_memset(&svc, 0, sizeof(svc));
    strcpy(svc.default_model, "engine-default");
    svc.default_max_output_tokens = 8000;

    char *models[] = {"m1", NULL};
    int caps[] = {16000, 0};
    provider_t prov;
    make_provider(&prov, models, caps);

    char model[128];
    int max_tokens = -1;
    run_resolve(&svc, &prov, "m1", 32000, model, sizeof(model), &max_tokens);
    check_int("clipped.max_tokens", max_tokens, 16000);
}

static void test_cap_beats_engine_default(void)
{
    llm_service_t svc;
    __builtin_memset(&svc, 0, sizeof(svc));
    strcpy(svc.default_model, "engine-default");
    svc.default_max_output_tokens = 8000;

    char *models[] = {"m1", NULL};
    int caps[] = {16000, 0};
    provider_t prov;
    make_provider(&prov, models, caps);

    char model[128];
    int max_tokens = -1;
    run_resolve(&svc, &prov, "m1", 0, model, sizeof(model), &max_tokens);
    check_int("cap_beats_default.max_tokens", max_tokens, 16000);
}

static void test_engine_default_fallback(void)
{
    llm_service_t svc;
    __builtin_memset(&svc, 0, sizeof(svc));
    strcpy(svc.default_model, "engine-default");
    svc.default_max_output_tokens = 8000;

    char *models[] = {"m1", NULL};
    int caps[] = {0, 0};
    provider_t prov;
    make_provider(&prov, models, caps);

    char model[128];
    int max_tokens = -1;
    run_resolve(&svc, &prov, "m1", 0, model, sizeof(model), &max_tokens);
    check_int("engine_default.max_tokens", max_tokens, 8000);
}

static void test_unset_stays_unset(void)
{
    llm_service_t svc;
    __builtin_memset(&svc, 0, sizeof(svc));
    strcpy(svc.default_model, "engine-default");
    svc.default_max_output_tokens = 0;

    char *models[] = {"m1", NULL};
    int caps[] = {0, 0};
    provider_t prov;
    make_provider(&prov, models, caps);

    char model[128];
    int max_tokens = -1;
    run_resolve(&svc, &prov, "m1", 0, model, sizeof(model), &max_tokens);
    check_int("unset.max_tokens", max_tokens, 0);
}

static void test_negative_intent_clamped(void)
{
    llm_service_t svc;
    __builtin_memset(&svc, 0, sizeof(svc));
    strcpy(svc.default_model, "engine-default");
    svc.default_max_output_tokens = 8000;

    char *models[] = {"m1", NULL};
    int caps[] = {16000, 0};
    provider_t prov;
    make_provider(&prov, models, caps);

    char model[128];
    int max_tokens = -1;
    run_resolve(&svc, &prov, "m1", -1, model, sizeof(model), &max_tokens);
    check_int("negative_intent_has_cap.max_tokens", max_tokens, 16000);

    llm_service_t svc2;
    __builtin_memset(&svc2, 0, sizeof(svc2));
    strcpy(svc2.default_model, "engine-default");
    svc2.default_max_output_tokens = 4096;

    char *models2[] = {"m1", NULL};
    int caps2[] = {0, 0};
    provider_t prov2;
    make_provider(&prov2, models2, caps2);

    run_resolve(&svc2, &prov2, "m1", -5, model, sizeof(model), &max_tokens);
    check_int("negative_intent_engine_default.max_tokens", max_tokens, 4096);

    llm_service_t svc3;
    __builtin_memset(&svc3, 0, sizeof(svc3));
    strcpy(svc3.default_model, "engine-default");
    svc3.default_max_output_tokens = 0;

    run_resolve(&svc3, &prov2, "m1", -5, model, sizeof(model), &max_tokens);
    check_int("negative_intent_no_cap.max_tokens", max_tokens, 0);
}

static void test_model_fallback(void)
{
    llm_service_t svc;
    __builtin_memset(&svc, 0, sizeof(svc));
    strcpy(svc.default_model, "engine-default");
    svc.default_max_output_tokens = 4096;

    char *models[] = {"m1", NULL};
    int caps[] = {16000, 0};
    provider_t prov;
    make_provider(&prov, models, caps);

    char model[128];
    int max_tokens = -1;
    run_resolve(&svc, &prov, NULL, 0, model, sizeof(model), &max_tokens);
    check_str("model_fallback.model", model, "engine-default");
    check_int("model_fallback.max_tokens", max_tokens, 4096);

    run_resolve(&svc, &prov, "", 0, model, sizeof(model), &max_tokens);
    check_str("model_fallback_empty.model", model, "engine-default");
}

static void test_null_provider(void)
{
    llm_service_t svc;
    __builtin_memset(&svc, 0, sizeof(svc));
    strcpy(svc.default_model, "engine-default");
    svc.default_max_output_tokens = 4096;

    char model[128];
    int max_tokens = -1;
    run_resolve(&svc, NULL, "m1", 0, model, sizeof(model), &max_tokens);
    check_int("null_provider.max_tokens", max_tokens, 4096);
    check_str("null_provider.model", model, "m1");
}

static void test_registry_model_cap_lookup(void)
{
    char *models[] = {"m1", "m2", NULL};
    int caps[] = {16000, 32768, 0};
    provider_t prov;
    make_provider(&prov, models, caps);

    check_int("lookup.m1", provider_registry_model_max_output(&prov, "m1"), 16000);
    check_int("lookup.m2", provider_registry_model_max_output(&prov, "m2"), 32768);
    check_int("lookup.miss", provider_registry_model_max_output(&prov, "m3"), 0);
    check_int("lookup.null_prov", provider_registry_model_max_output(NULL, "m1"), 0);
    check_int("lookup.null_model", provider_registry_model_max_output(&prov, NULL), 0);

    provider_t no_caps;
    __builtin_memset(&no_caps, 0, sizeof(no_caps));
    no_caps.models = models;
    no_caps.model_max_output = NULL;
    check_int("lookup.no_table", provider_registry_model_max_output(&no_caps, "m1"), 0);
}

int main(void)
{
    printf("running resolve_gen_params / model-cap regression\n");
    test_priority_within_cap();
    test_explicit_clipped_by_cap();
    test_cap_beats_engine_default();
    test_engine_default_fallback();
    test_unset_stays_unset();
    test_negative_intent_clamped();
    test_model_fallback();
    test_null_provider();
    test_registry_model_cap_lookup();

    if (fail_count == 0) {
        printf("All %d gen-params assertions passed\n", test_count);
        return 0;
    }
    printf("%d/%d gen-params assertions failed\n", fail_count, test_count);
    return 1;
}
