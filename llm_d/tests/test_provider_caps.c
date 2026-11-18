// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_provider_caps.c
 * @brief 0.1.21 B-1：provider 能力位匹配与按能力选路回归。
 *
 * 三组能力位组合（加速器+流式+工具 / 显存下限 / 仅工具）必须命中预期
 * provider；未声明能力位的候选按保守可用参与，显式不满足者被跳过；
 * 空要求退化为注册顺序（旧「按名字取第一个」行为）。
 */

#include "airy_prov/provider_caps.h"
#include "airy_prov/registry.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_fail++;                                                            \
        }                                                                        \
    } while (0)

static provider_caps_t caps_make(airy_accel_kind_t accel, uint64_t vram, uint32_t ctx,
                                 int stream, int tools)
{
    provider_caps_t c;

    memset(&c, 0, sizeof(c));
    c.declared = 1;
    c.accelerator = accel;
    c.vram_bytes = vram;
    c.max_context = ctx;
    c.supports_stream = stream;
    c.supports_tools = tools;
    return c;
}

static void test_match(void)
{
    provider_caps_t intel = caps_make(AIRY_ACCEL_INTEL, 8ULL << 30, 8192, 1, 1);
    provider_caps_t unknown;
    provider_select_t q;

    memset(&unknown, 0, sizeof(unknown));
    memset(&q, 0, sizeof(q));
    q.require_accel = AIRY_ACCEL_INTEL;
    q.need_stream = 1;
    q.need_tools = 1;
    q.min_context = 4096;
    CHECK(provider_caps_match(&intel, &q) == PROVIDER_CAPS_EXACT);
    CHECK(provider_caps_match(&unknown, &q) == PROVIDER_CAPS_CONSERVATIVE);
    CHECK(provider_caps_match(NULL, &q) == PROVIDER_CAPS_CONSERVATIVE);

    q.require_accel = AIRY_ACCEL_NVIDIA;
    CHECK(provider_caps_match(&intel, &q) == PROVIDER_CAPS_MISS);

    memset(&q, 0, sizeof(q));
    q.need_stream = 1;
    CHECK(provider_caps_match(NULL, &q) == PROVIDER_CAPS_CONSERVATIVE);
    CHECK(provider_caps_match(NULL, NULL) == PROVIDER_CAPS_EXACT);
    CHECK(provider_caps_match(&unknown, NULL) == PROVIDER_CAPS_EXACT);
    CHECK(provider_caps_match(&intel, NULL) == PROVIDER_CAPS_EXACT);

    intel.supports_tools = 0;
    q.need_tools = 1;
    CHECK(provider_caps_match(&intel, &q) == PROVIDER_CAPS_MISS);

    /* 已声明但个别字段未知（-1 / 0）时，只对未知字段保守，不否决已知项。 */
    intel.supports_tools = 1;
    intel.max_context = 0;
    q.need_tools = 0;
    q.min_context = 4096;
    CHECK(provider_caps_match(&intel, &q) == PROVIDER_CAPS_CONSERVATIVE);
}

static provider_registry_t *build_registry(service_config_t *cfg, provider_config_t *provs,
                                           char *a_models[], char *b_models[],
                                           char *c_models[])
{
    memset(cfg, 0, sizeof(*cfg));
    memset(provs, 0, 3 * sizeof(*provs));
    provs[0].name = "alpha";
    provs[0].api_format = "chat";
    provs[0].api_base = "http://127.0.0.1:9/v1";
    provs[0].models = a_models;
    provs[0].hw = caps_make(AIRY_ACCEL_INTEL, 8ULL << 30, 8192, 1, 1);
    provs[1].name = "beta";
    provs[1].api_format = "chat";
    provs[1].api_base = "http://127.0.0.1:9/v1";
    provs[1].models = b_models;
    provs[1].hw = caps_make(AIRY_ACCEL_NVIDIA, 16ULL << 30, 32768, 1, 0);
    provs[2].name = "gamma";
    provs[2].api_format = "chat";
    provs[2].api_base = "http://127.0.0.1:9/v1";
    provs[2].models = c_models;
    cfg->providers = provs;
    cfg->provider_count = 3;
    return provider_registry_create(cfg);
}

static void test_select(void)
{
    char *a_models[] = {"shared", NULL};
    char *b_models[] = {"shared", "nvidia-only", NULL};
    char *c_models[] = {"shared", NULL};
    service_config_t cfg;
    provider_config_t provs[3];
    provider_registry_t *reg = build_registry(&cfg, provs, a_models, b_models, c_models);
    provider_select_t q;
    const provider_t *p;

    CHECK(reg != NULL);
    if (!reg)
        return;

    /* 组合一：加速器 + 流式 + 工具。 */
    memset(&q, 0, sizeof(q));
    q.model = "shared";
    q.require_accel = AIRY_ACCEL_INTEL;
    q.need_stream = 1;
    q.need_tools = 1;
    q.min_context = 4096;
    p = provider_select(reg, &q);
    CHECK(p && strcmp(p->name, "alpha") == 0);

    /* 组合二：显存下限 + 加速器。 */
    memset(&q, 0, sizeof(q));
    q.model = "shared";
    q.require_accel = AIRY_ACCEL_NVIDIA;
    q.min_vram_bytes = 12ULL << 30;
    p = provider_select(reg, &q);
    CHECK(p && strcmp(p->name, "beta") == 0);

    /* 组合三：仅工具；beta 显式不支持，gamma 未知按保守可用。 */
    memset(&q, 0, sizeof(q));
    q.model = "shared";
    q.need_tools = 1;
    p = provider_select(reg, &q);
    CHECK(p && strcmp(p->name, "alpha") == 0);

    /* 显式不满足（无条件声明工具）的 provider 不得命中。 */
    memset(&q, 0, sizeof(q));
    q.model = "shared";
    q.need_tools = 1;
    q.require_accel = AIRY_ACCEL_NVIDIA;
    p = provider_select(reg, &q);
    CHECK(p && strcmp(p->name, "gamma") == 0);

    /* 空要求：注册顺序第一个（旧行为）。 */
    memset(&q, 0, sizeof(q));
    p = provider_select(reg, &q);
    CHECK(p && strcmp(p->name, "alpha") == 0);
    CHECK(provider_select(reg, NULL) != NULL);
    CHECK(strcmp(provider_select(reg, NULL)->name, "alpha") == 0);

    /* 模型不在表内：无候选，返回 NULL 而非崩溃。 */
    memset(&q, 0, sizeof(q));
    q.model = "nope";
    q.need_stream = 1;
    CHECK(provider_select(reg, &q) == NULL);

    /* 显式无工具声明的模型专属 provider：工具要求下无候选。 */
    memset(&q, 0, sizeof(q));
    q.model = "nvidia-only";
    q.need_tools = 1;
    CHECK(provider_select(reg, &q) == NULL);

    /* 精确查找不受能力位影响（旧入口保持）。 */
    CHECK(provider_registry_find(reg, "nvidia-only") != NULL);

    provider_registry_destroy(reg);
}

int main(void)
{
    test_match();
    test_select();
    if (g_fail) {
        fprintf(stderr, "test_provider_caps: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_provider_caps: PASS\n");
    return 0;
}
