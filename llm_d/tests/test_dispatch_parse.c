// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_dispatch_parse.c
 * @brief T1 前缀稳定化与 T2 槽位亲和的解析域回归（parse_params 公共入口）
 *
 * 锁定请求解析域五条不变量：
 *   1. 前缀稳定（缺省）：时间上下文以独立 system 消息追加在尾部，原始
 *      首条原样保留——连续两轮解析的首条逐字节相同是服务端 KV 缓存
 *      前缀命中的前置条件
 *   2. 回退开关：AIRY_LLM_PREFIX_STABLE=off|0 恢复 slot 0 注入
 *      （0.1.19 行为），原始消息整体后移
 *   3. 首条 system 已带日期戳即短路注入（两种模式一致），不重复补注
 *   4. slot_id 仅显式数字才置哨兵；槽位 0 合法可表达
 *   5. 满载守恒：上限条原始消息在任一注入模式下必须整请求拒绝，
 *      不得静默丢弃时间感知
 *   6. C3 thinking：显式传 chat_template_kwargs.enable_thinking（含
 *      false）才置哨兵；budget 可选随行；缺省不置（旧请求体不变）
 */

#include "llm_d_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 装配域单例不在测试链接集内（svc.c 随主可执行目标构建），由测试自持；
 * 用例恒显式给 model，parse_model 不会解引用该指针。 */
llm_service_t *g_service = NULL;

static int test_count = 0;
static int fail_count = 0;

static void check_true(const char *name, int cond)
{
    test_count++;
    if (!cond) {
        printf("    FAIL: %s\n", name);
        fail_count++;
    }
}

static void check_int(const char *name, long got, long want)
{
    test_count++;
    if (got != want) {
        printf("    FAIL: %s: got %ld, want %ld\n", name, got, want);
        fail_count++;
    }
}

static void check_str(const char *name, const char *got, const char *want)
{
    test_count++;
    if (!got || strcmp(got, want) != 0) {
        printf("    FAIL: %s: got \"%s\", want \"%s\"\n", name,
               got ? got : "(null)", want);
        fail_count++;
    }
}

static void env_stable_reset(void)
{
    unsetenv("AIRY_LLM_PREFIX_STABLE");
}

/* 最小 params：显式 model + system/user 两条消息。 */
static cJSON *make_params(const char *sys, const char *user)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *msgs;
    cJSON *m;

    cJSON_AddStringToObject(params, "model", "m1");
    msgs = cJSON_AddArrayToObject(params, "messages");
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", sys);
    cJSON_AddItemToArray(msgs, m);
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", user);
    cJSON_AddItemToArray(msgs, m);
    return params;
}

/* 解析驱动：成功返回 ctx（调用方 destroy），失败回收后返回 NULL。 */
static request_context_t *run_parse(cJSON *params, llm_request_config_t *cfg, int *rc)
{
    request_context_t *ctx = request_context_create();

    memset(cfg, 0, sizeof(*cfg));
    *rc = ctx ? parse_params(params, ctx, cfg) : -1;
    if (*rc != 0) {
        request_context_destroy(ctx);
        return NULL;
    }
    return ctx;
}

/* 缺省（stable）模式：原始两条消息原样在前，时间上下文以尾部 system
 * 追加，首条不被篡改。 */
static void test_stable_tail_default(void)
{
    llm_request_config_t cfg;
    int rc;

    env_stable_reset();
    cJSON *params = make_params("be helpful", "hello");
    check_true("stable.root", params != NULL);
    if (!params)
        return;
    request_context_t *ctx = run_parse(params, &cfg, &rc);
    check_int("stable.rc", rc, 0);
    if (ctx) {
        check_int("stable.count", (long)ctx->message_count, 3);
        check_int("stable.cfg_count", (long)cfg.message_count, 3);
        if (ctx->message_count == 3) {
            check_str("stable.first_role", ctx->messages[0].role, "system");
            check_str("stable.first_content", ctx->messages[0].content, "be helpful");
            check_str("stable.second_role", ctx->messages[1].role, "user");
            check_str("stable.second_content", ctx->messages[1].content, "hello");
            check_str("stable.tail_role", ctx->messages[2].role, "system");
            check_true("stable.tail_time", ctx->messages[2].content &&
                           strstr(ctx->messages[2].content, "当前时间：") != NULL);
        }
        request_context_destroy(ctx);
    }
    cJSON_Delete(params);
}

/* T1 核心不变量：同一会话连续两轮，请求首条逐字节相同（前缀稳定）。
 * 尾部时间消息不参与前缀判定——两轮时间漂移合法。 */
static void test_prefix_stable_across_rounds(void)
{
    llm_request_config_t cfg;
    int rc;

    env_stable_reset();
    cJSON *p1 = make_params("be helpful", "round one");
    cJSON *p2 = make_params("be helpful", "round two");
    check_true("prefix.params", p1 && p2);
    if (p1 && p2) {
        request_context_t *c1 = run_parse(p1, &cfg, &rc);
        request_context_t *c2 = run_parse(p2, &cfg, &rc);
        check_true("prefix.rounds", c1 && c2);
        if (c1 && c2)
            check_true("prefix.first_identical",
                       c1->messages[0].content && c2->messages[0].content &&
                           strcmp(c1->messages[0].content,
                                  c2->messages[0].content) == 0);
        request_context_destroy(c1);
        request_context_destroy(c2);
    }
    cJSON_Delete(p1);
    cJSON_Delete(p2);
}

/* 首条 system 已带日期戳即短路注入：stable 不追加尾条，回退不占 slot 0。 */
static void test_has_time_short_circuit(void)
{
    llm_request_config_t cfg;
    int rc;
    const char *stamped =
        "当前时间：2026-01-01 星期四 00:00（宿主机系统时间） be helpful";

    env_stable_reset();
    cJSON *params = make_params(stamped, "hello");
    request_context_t *ctx = run_parse(params, &cfg, &rc);
    check_int("short.stable_rc", rc, 0);
    if (ctx) {
        check_int("short.stable_count", (long)ctx->message_count, 2);
        if (ctx->message_count == 2)
            check_str("short.stable_first", ctx->messages[0].content, stamped);
        request_context_destroy(ctx);
    }
    cJSON_Delete(params);

    setenv("AIRY_LLM_PREFIX_STABLE", "off", 1);
    params = make_params(stamped, "hello");
    ctx = run_parse(params, &cfg, &rc);
    check_int("short.fallback_rc", rc, 0);
    if (ctx) {
        check_int("short.fallback_count", (long)ctx->message_count, 2);
        if (ctx->message_count == 2)
            check_str("short.fallback_first", ctx->messages[0].content, stamped);
        request_context_destroy(ctx);
    }
    cJSON_Delete(params);
    env_stable_reset();
}

/* 回退开关：off|0 双写法均恢复 slot 0 注入（0.1.19 行为），原始消息
 * 整体后移。 */
static void test_fallback_slot0(void)
{
    llm_request_config_t cfg;
    int rc;

    setenv("AIRY_LLM_PREFIX_STABLE", "off", 1);
    cJSON *params = make_params("be helpful", "hello");
    request_context_t *ctx = run_parse(params, &cfg, &rc);
    check_int("fb.rc", rc, 0);
    if (ctx) {
        check_int("fb.count", (long)ctx->message_count, 3);
        if (ctx->message_count == 3) {
            check_str("fb.slot0_role", ctx->messages[0].role, "system");
            check_true("fb.slot0_time", ctx->messages[0].content &&
                           strstr(ctx->messages[0].content, "当前时间：") != NULL);
            check_str("fb.orig_first", ctx->messages[1].content, "be helpful");
            check_str("fb.orig_second", ctx->messages[2].content, "hello");
        }
        request_context_destroy(ctx);
    }
    cJSON_Delete(params);

    setenv("AIRY_LLM_PREFIX_STABLE", "0", 1);
    params = make_params("be helpful", "hello");
    ctx = run_parse(params, &cfg, &rc);
    check_int("fb0.rc", rc, 0);
    if (ctx) {
        check_int("fb0.count", (long)ctx->message_count, 3);
        if (ctx->message_count == 3)
            check_true("fb0.slot0_time", ctx->messages[0].content &&
                           strstr(ctx->messages[0].content, "当前时间：") != NULL);
        request_context_destroy(ctx);
    }
    cJSON_Delete(params);
    env_stable_reset();
}

/* T2 slot_id 解析：显式数字才置哨兵，槽位 0 合法，缺省保持未设置。 */
static void test_slot_id_roundtrip(void)
{
    llm_request_config_t cfg;
    int rc;

    env_stable_reset();

    cJSON *params = make_params("be helpful", "hello");
    cJSON_AddNumberToObject(params, "slot_id", 2);
    request_context_t *ctx = run_parse(params, &cfg, &rc);
    check_int("slot.rc", rc, 0);
    check_true("slot.set", cfg.slot_set);
    check_int("slot.value", cfg.slot_id, 2);
    request_context_destroy(ctx);
    cJSON_Delete(params);

    params = make_params("be helpful", "hello");
    cJSON_AddNumberToObject(params, "slot_id", 0);
    ctx = run_parse(params, &cfg, &rc);
    check_int("slot0.rc", rc, 0);
    check_true("slot0.set", cfg.slot_set);
    check_int("slot0.value", cfg.slot_id, 0);
    request_context_destroy(ctx);
    cJSON_Delete(params);

    params = make_params("be helpful", "hello");
    ctx = run_parse(params, &cfg, &rc);
    check_int("noslot.rc", rc, 0);
    check_true("noslot.unset", !cfg.slot_set);
    request_context_destroy(ctx);
    cJSON_Delete(params);
}

/* C3 thinking 解析：enable_thinking 为意图哨兵（false 合法可表达），
 * budget 可选随行；缺省不置哨兵保持旧请求体字节级不变。 */
static void test_thinking_roundtrip(void)
{
    llm_request_config_t cfg;
    int rc;
    cJSON *params;
    cJSON *ctk;
    request_context_t *ctx;

    env_stable_reset();

    params = make_params("be helpful", "hello");
    ctk = cJSON_AddObjectToObject(params, "chat_template_kwargs");
    cJSON_AddBoolToObject(ctk, "enable_thinking", 0);
    ctx = run_parse(params, &cfg, &rc);
    check_int("think.rc", rc, 0);
    check_true("think.set", cfg.thinking_set);
    check_true("think.off", !cfg.enable_thinking);
    check_int("think.nobudget", (long)cfg.thinking_budget, 0);
    request_context_destroy(ctx);
    cJSON_Delete(params);

    params = make_params("be helpful", "hello");
    ctk = cJSON_AddObjectToObject(params, "chat_template_kwargs");
    cJSON_AddBoolToObject(ctk, "enable_thinking", 1);
    cJSON_AddNumberToObject(ctk, "thinking_budget", 128);
    ctx = run_parse(params, &cfg, &rc);
    check_int("thinkon.rc", rc, 0);
    check_true("thinkon.set", cfg.thinking_set);
    check_true("thinkon.on", cfg.enable_thinking);
    check_int("thinkon.budget", (long)cfg.thinking_budget, 128);
    request_context_destroy(ctx);
    cJSON_Delete(params);

    params = make_params("be helpful", "hello");
    ctx = run_parse(params, &cfg, &rc);
    check_int("nothink.rc", rc, 0);
    check_true("nothink.unset", !cfg.thinking_set);
    request_context_destroy(ctx);
    cJSON_Delete(params);

    env_stable_reset();
}

/* 满载守恒：上限条原始消息在任一注入模式下都无处安放时间上下文，
 * 必须整请求拒绝并给出具体原因，不得静默丢弃时间感知。 */
static void test_overflow_rejected(void)
{
    llm_request_config_t cfg;
    int rc;

    env_stable_reset();
    for (int mode = 0; mode < 2; ++mode) {
        if (mode == 1)
            setenv("AIRY_LLM_PREFIX_STABLE", "off", 1);
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "model", "m1");
        cJSON *msgs = cJSON_AddArrayToObject(params, "messages");
        for (int i = 0; i < MAX_MESSAGES_PER_REQUEST; ++i) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "user");
            cJSON_AddStringToObject(m, "content", "x");
            cJSON_AddItemToArray(msgs, m);
        }
        request_context_t *ctx = request_context_create();
        memset(&cfg, 0, sizeof(cfg));
        rc = (ctx && params && msgs) ? parse_params(params, ctx, &cfg) : -1;
        check_true(mode ? "full.fallback_rejected" : "full.stable_rejected", rc != 0);
        if (ctx) {
            check_true(mode ? "full.fallback_reason" : "full.stable_reason",
                       ctx->fail_reason[0] != '\0');
            request_context_destroy(ctx);
        }
        cJSON_Delete(params);
    }
    env_stable_reset();
}

int main(void)
{
    printf("running T1/T2 dispatch parse regression\n");
    test_stable_tail_default();
    test_prefix_stable_across_rounds();
    test_has_time_short_circuit();
    test_fallback_slot0();
    test_slot_id_roundtrip();
    test_thinking_roundtrip();
    test_overflow_rejected();

    if (fail_count == 0) {
        printf("All %d dispatch-parse assertions passed\n", test_count);
        return 0;
    }
    printf("%d/%d dispatch-parse assertions failed\n", fail_count, test_count);
    return 1;
}
