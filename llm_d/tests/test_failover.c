// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_failover.c
 * @brief 0.1.19 §3.8.2 端点切换决策件回归
 *
 * 锁定「错误码 → 可否换端点」这一唯一判据，重点覆盖三处 class 归类
 * 不足以表达的显式前置，防止后续重构退回纯 class 推导：
 *
 *   1. AIRY_ERR_IO 未入 err_class 表（落 business），仍须判 transport
 *   2. AIRY_ERR_LLM_AUTH_FAIL 归 config，仍须判 auth
 *   3. AIRY_ERR_LLM_RATE_LIMIT 归 transport，仍须独立成类
 *   4. BAD_REQUEST / 参数类 / 策略类 永不切换
 *   5. 白名单掩码语义与原因名双向映射
 */

#include "router/core/llm_failover.h"

#include "error_codes.h"

#include <stdio.h>
#include <string.h>

static int test_count = 0;
static int fail_count = 0;

static void check_reason(const char *name, int err_code, llm_fo_reason_t want)
{
    llm_fo_reason_t got = llm_fo_reason(err_code);

    test_count++;
    if (got != want) {
        printf("    FAIL: %s: err=%d got %s, want %s\n", name, err_code, llm_fo_name(got),
               llm_fo_name(want));
        fail_count++;
    }
}

static void check_should(const char *name, int err_code, unsigned mask, int want)
{
    int got = llm_fo_should(err_code, mask);

    test_count++;
    if (got != want) {
        printf("    FAIL: %s: err=%d mask=0x%x got %d, want %d\n", name, err_code, mask, got,
               want);
        fail_count++;
    }
}

static void check_name(const char *name, llm_fo_reason_t reason, const char *want)
{
    const char *got = llm_fo_name(reason);

    test_count++;
    if (got == NULL || strcmp(got, want) != 0) {
        printf("    FAIL: %s: got \"%s\", want \"%s\"\n", name, got ? got : "(null)", want);
        fail_count++;
    }
}

/* 三处显式前置：class 归类会给出错误答案，必须由显式表命中。 */
static void test_explicit(void)
{
    check_reason("io_is_transport", AIRY_ERR_IO, LLM_FO_TRANSPORT);
    check_reason("auth_is_auth", AIRY_ERR_LLM_AUTH_FAIL, LLM_FO_AUTH);
    check_reason("rate_is_rate", AIRY_ERR_LLM_RATE_LIMIT, LLM_FO_RATE_LIMIT);
    check_should("io_switchable", AIRY_ERR_IO, LLM_FO_ON_DEFAULT, 1);
    check_should("auth_switchable", AIRY_ERR_LLM_AUTH_FAIL, LLM_FO_ON_DEFAULT, 1);
    check_should("rate_switchable", AIRY_ERR_LLM_RATE_LIMIT, LLM_FO_ON_DEFAULT, 1);
}

/* 链路类（含 class=transport 兜底）全部可切。 */
static void test_transport(void)
{
    check_reason("timeout", AIRY_ERR_TIMEOUT, LLM_FO_TRANSPORT);
    check_reason("no_provider", AIRY_ERR_LLM_NO_PROVIDER, LLM_FO_TRANSPORT);
    check_reason("provider_fail", AIRY_ERR_LLM_PROVIDER_FAIL, LLM_FO_TRANSPORT);
    check_should("timeout_switchable", AIRY_ERR_TIMEOUT, LLM_FO_ON_DEFAULT, 1);
}

/* 请求本身非法或策略拒绝：换端点必然同样失败，恒不切。 */
static void test_never(void)
{
    check_reason("bad_request", AIRY_ERR_LLM_BAD_REQUEST, LLM_FO_NONE);
    check_reason("invalid_model", AIRY_ERR_LLM_INVALID_MODEL, LLM_FO_NONE);
    check_reason("context_len", AIRY_ERR_LLM_CONTEXT_LEN, LLM_FO_NONE);
    check_reason("token_limit", AIRY_ERR_LLM_TOKEN_LIMIT, LLM_FO_NONE);
    check_reason("cost_exceed", AIRY_ERR_LLM_COST_EXCEED, LLM_FO_NONE);
    check_reason("policy_denied", AIRY_ERR_PERMISSION_DENIED, LLM_FO_NONE);
    check_reason("zero", 0, LLM_FO_NONE);
    check_should("bad_request_blocked", AIRY_ERR_LLM_BAD_REQUEST, LLM_FO_ON_DEFAULT, 0);
    check_should("policy_blocked", AIRY_ERR_PERMISSION_DENIED, LLM_FO_ON_DEFAULT, 0);
}

/* 正号入参须与负号等价：上游不同层可能已取过绝对值。 */
static void test_sign(void)
{
    check_reason("pos_auth", -AIRY_ERR_LLM_AUTH_FAIL, LLM_FO_AUTH);
    check_reason("pos_io", -AIRY_ERR_IO, LLM_FO_TRANSPORT);
}

/* 白名单掩码：配置面 `on:` 收窄后必须立刻生效。 */
static void test_mask(void)
{
    check_should("auth_only_auth", AIRY_ERR_LLM_AUTH_FAIL, LLM_FO_BIT(LLM_FO_AUTH), 1);
    check_should("auth_only_rate", AIRY_ERR_LLM_RATE_LIMIT, LLM_FO_BIT(LLM_FO_AUTH), 0);
    check_should("empty_mask", AIRY_ERR_IO, 0U, 0);
    check_should("all_mask_none", AIRY_ERR_LLM_BAD_REQUEST, 0xFFFFFFFFU, 0);

    test_count++;
    if (LLM_FO_ON_DEFAULT !=
        (LLM_FO_BIT(LLM_FO_TRANSPORT) | LLM_FO_BIT(LLM_FO_RATE_LIMIT) |
         LLM_FO_BIT(LLM_FO_AUTH))) {
        printf("    FAIL: default mask drifted from §3.8.2 on-list\n");
        fail_count++;
    }
}

/* 原因名是配置面与日志面的稳定契约，必须可逆。 */
static void test_names(void)
{
    llm_fo_reason_t r;

    check_name("name_none", LLM_FO_NONE, "none");
    check_name("name_transport", LLM_FO_TRANSPORT, "transport");
    check_name("name_rate", LLM_FO_RATE_LIMIT, "rate_limit");
    check_name("name_auth", LLM_FO_AUTH, "auth");

    for (r = LLM_FO_NONE; r <= LLM_FO_AUTH; ++r) {
        test_count++;
        if (llm_fo_parse(llm_fo_name(r)) != r) {
            printf("    FAIL: roundtrip broken for %s\n", llm_fo_name(r));
            fail_count++;
        }
    }

    test_count++;
    if (llm_fo_parse("none") != LLM_FO_NONE) {
        printf("    FAIL: \"none\" must map to none\n");
        fail_count++;
    }
    test_count++;
    if (llm_fo_parse("bogus") != LLM_FO_NONE) {
        printf("    FAIL: unknown name must map to none\n");
        fail_count++;
    }
    test_count++;
    if (llm_fo_parse(NULL) != LLM_FO_NONE) {
        printf("    FAIL: NULL name must map to none\n");
        fail_count++;
    }
}

int main(void)
{
    printf("running route failover decision regression\n");

    test_explicit();
    test_transport();
    test_never();
    test_sign();
    test_mask();
    test_names();

    if (fail_count == 0) {
        printf("All %d failover assertions passed\n", test_count);
        return 0;
    }
    printf("%d/%d failover assertions failed\n", fail_count, test_count);
    return 1;
}
