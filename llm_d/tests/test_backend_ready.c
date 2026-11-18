/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file test_backend_ready.c
 * @brief 0.1.21 B-2：后端就绪判定与未配置快速失败回归。
 *
 * 三组配置形态覆盖扫描结论（未声明加速器 / 已声明且有地址 / 已声明无地址）；
 * 驱动面用假适配器证明未配置时 build_request 零调用（无请求体装配、无网络
 * I/O）、错误码为 AIRY_ERR_LLM_BACKEND_UNCONFIGURED，且在 5s 预算内返回；
 * 就绪时不得被误拦，仍走原有装配路径。
 */

#include "airy_prov/adapter.h"
#include "airy_prov/provider_backend.h"
#include "error_codes.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static int g_fail;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            g_fail++;                                                                \
        }                                                                            \
    } while (0)

static provider_config_t cfg_make(int declared, airy_accel_kind_t accel, const char *api_base)
{
    provider_config_t c = {0};
    c.name = "gpu";
    c.hw.declared = declared;
    c.hw.accelerator = accel;
    c.api_base = api_base;
    return c;
}

static void test_scan(void)
{
    provider_backend_t be;

    /* 未声明能力位：按旧路径可用，且不宣称任何后端类别。 */
    provider_config_t c = cfg_make(0, AIRY_ACCEL_NONE, NULL);
    prov_be_scan(&c, &be);
    CHECK(be.ready == 1);
    CHECK(be.kind == PROVIDER_BACKEND_KIND_NONE);
    CHECK(be.reason[0] == '\0');

    /* 加速器未知（declared=1 但 NONE）：仍按旧路径可用。 */
    c = cfg_make(1, AIRY_ACCEL_NONE, NULL);
    prov_be_scan(&c, &be);
    CHECK(be.ready == 1);

    /* 声明具体加速器且给出地址：后端就绪，类别为 OpenAI 兼容。 */
    c = cfg_make(1, AIRY_ACCEL_INTEL, "http://127.0.0.1:8080/v1");
    prov_be_scan(&c, &be);
    CHECK(be.ready == 1);
    CHECK(be.kind == PROVIDER_BACKEND_KIND_STDAPI);

    /* 声明具体加速器但缺地址：未配置，原因串必须非空。 */
    c = cfg_make(1, AIRY_ACCEL_INTEL, NULL);
    prov_be_scan(&c, &be);
    CHECK(be.ready == 0);
    CHECK(be.kind == PROVIDER_BACKEND_KIND_STDAPI);
    CHECK(be.reason[0] != '\0');

    /* 空串与 NULL 同义。 */
    c = cfg_make(1, AIRY_ACCEL_NVIDIA, "");
    prov_be_scan(&c, &be);
    CHECK(be.ready == 0);
}

/* 假适配器：只记装配调用次数，不产请求体。 */
static int g_build_calls;

static char *fake_build(const llm_request_config_t *cfg)
{
    (void)cfg;
    g_build_calls++;
    return NULL;
}

static int fake_parse(const char *data, llm_response_t **out)
{
    (void)data;
    if (out)
        *out = NULL;
    return 0;
}

static int fake_stream_cb(const char *chunk, void *user_data)
{
    (void)chunk;
    (void)user_data;
    return 0;
}

typedef struct {
    provider_base_ctx_t base;
} fake_ctx_t;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void test_gate(void)
{
    fake_ctx_t fctx = {0};
    snprintf(fctx.base.backend_reason, sizeof(fctx.base.backend_reason), "api_base 缺失");

    provider_adapter_t ops = {0};
    ops.tag = "fake";
    ops.chat_path = "/v1/chat/completions";
    ops.build_request = fake_build;
    ops.parse_response = fake_parse;

    llm_request_config_t req = {0};
    req.model = "gpu-model";

    llm_response_t *resp = NULL;
    uint64_t t0 = now_ms();
    int rc = provider_driver_complete((provider_ctx_t *)&fctx, &ops, &req, &resp);
    uint64_t elapsed = now_ms() - t0;

    CHECK(rc == AIRY_ERR_LLM_BACKEND_UNCONFIGURED);
    CHECK(g_build_calls == 0);
    CHECK(elapsed < PROVIDER_BACKEND_FAILFAST_MS);

    /* 就绪时不得被误拦：假适配器返回 NULL 体，应落到装配失败分支。 */
    fctx.base.backend_ready = 1;
    g_build_calls = 0;
    rc = provider_driver_complete((provider_ctx_t *)&fctx, &ops, &req, &resp);
    CHECK(g_build_calls == 1);
    CHECK(rc == AIRY_ERR_OUT_OF_MEMORY);

    /* 流式入口同一判据：未配置时不得进入回调，也不得装配请求体。 */
    fctx.base.backend_ready = 0;
    g_build_calls = 0;
    rc = provider_driver_complete_stream((provider_ctx_t *)&fctx, &ops, &req, fake_stream_cb,
                                         NULL, &resp);
    CHECK(rc == AIRY_ERR_LLM_BACKEND_UNCONFIGURED);
    CHECK(g_build_calls == 0);
}

int main(void)
{
    test_scan();
    test_gate();

    if (g_fail) {
        fprintf(stderr, "test_backend_ready: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("test_backend_ready: all checks passed\n");
    return 0;
}
