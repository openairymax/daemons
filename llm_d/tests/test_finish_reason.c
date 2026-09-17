// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_finish_reason.c
 * @brief C-4 (D) finish_reason 归一化 SSoT 回归测试
 *
 * 锁定全栈 canonical 词汇表（llm_service_types.h）这一唯一权威：
 *
 *   1. 各 provider 原生标记在边界映射为 stop/length/content_filter/tool_calls
 *   2. canonical 标记原样通过（幂等）
 *   3. 未识别标记原样保留，不丢上游信号
 *   4. NULL / 空串 → stop（norm 永不返回 NULL）
 *   5. 截断判定唯一入口 llm_finish_is_truncated()，原始标记不得直接判定
 *   6. wire 契约 response_to_json/response_from_json 双向透传 canonical 值
 */

#include "llm_service.h"
#include "response.h"

#include <stdio.h>
#include <string.h>

static int test_count = 0;
static int fail_count = 0;

static void check_str(const char *name, const char *got, const char *want)
{
    test_count++;
    if (!got || strcmp(got, want) != 0) {
        printf("    FAIL: %s: got \"%s\", want \"%s\"\n", name, got ? got : "(null)", want);
        fail_count++;
    }
}

static void check_int(const char *name, int got, int want)
{
    test_count++;
    if (got != want) {
        printf("    FAIL: %s: got %d, want %d\n", name, got, want);
        fail_count++;
    }
}

/* OpenAI 兼容面（OpenAI / DeepSeek / 本地端点）原生标记 */
static void test_norm_openai(void)
{
    check_str("openai.stop", llm_finish_reason_norm("stop"), LLM_FINISH_STOP);
    check_str("openai.length", llm_finish_reason_norm("length"), LLM_FINISH_LENGTH);
    check_str("openai.content_filter", llm_finish_reason_norm("content_filter"),
              LLM_FINISH_CONTENT_FILTER);
    check_str("openai.tool_calls", llm_finish_reason_norm("tool_calls"), LLM_FINISH_TOOL_CALLS);
}

/* Anthropic stop_reason 原生标记 */
static void test_norm_anthropic(void)
{
    check_str("anthropic.end_turn", llm_finish_reason_norm("end_turn"), LLM_FINISH_STOP);
    check_str("anthropic.stop_sequence", llm_finish_reason_norm("stop_sequence"), LLM_FINISH_STOP);
    check_str("anthropic.max_tokens", llm_finish_reason_norm("max_tokens"), LLM_FINISH_LENGTH);
    check_str("anthropic.tool_use", llm_finish_reason_norm("tool_use"), LLM_FINISH_TOOL_CALLS);
    check_str("anthropic.refusal", llm_finish_reason_norm("refusal"), LLM_FINISH_CONTENT_FILTER);
}

/* Google finishReason 原生标记（含此前流式路径漏映射的 SAFETY） */
static void test_norm_google(void)
{
    check_str("google.STOP", llm_finish_reason_norm("STOP"), LLM_FINISH_STOP);
    check_str("google.MAX_TOKENS", llm_finish_reason_norm("MAX_TOKENS"), LLM_FINISH_LENGTH);
    check_str("google.SAFETY", llm_finish_reason_norm("SAFETY"), LLM_FINISH_CONTENT_FILTER);
}

/* 未识别标记必须原样保留，避免吞掉上游新语义 */
static void test_norm_passthrough(void)
{
    check_str("pass.recitation", llm_finish_reason_norm("RECITATION"), "RECITATION");
    check_str("pass.malformed", llm_finish_reason_norm("MALFORMED_FUNCTION_CALL"),
              "MALFORMED_FUNCTION_CALL");
    check_str("pass.idempotent", llm_finish_reason_norm(LLM_FINISH_TOOL_CALLS),
              LLM_FINISH_TOOL_CALLS);
}

/* norm 永不返回 NULL：缺失标记回落 stop，且顺序相反的情况归一为截断 */
static void test_norm_missing(void)
{
    check_str("missing.null", llm_finish_reason_norm(NULL), LLM_FINISH_STOP);
    check_str("missing.empty", llm_finish_reason_norm(""), LLM_FINISH_STOP);
}

/* 截断判定唯一入口：原始 provider 标记不得直接判定 */
static void test_truncated(void)
{
    check_int("trunc.length", llm_finish_is_truncated(LLM_FINISH_LENGTH), 1);
    check_int("trunc.stop", llm_finish_is_truncated(LLM_FINISH_STOP), 0);
    check_int("trunc.tool_calls", llm_finish_is_truncated(LLM_FINISH_TOOL_CALLS), 0);
    check_int("trunc.null", llm_finish_is_truncated(NULL), 0);
    check_int("trunc.raw_marker", llm_finish_is_truncated("max_tokens"), 0);
}

/* wire 契约：canonical 值经 response JSON 双向透传不丢失 */
static void test_wire_roundtrip(void)
{
    llm_response_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.finish_reason = (char *)LLM_FINISH_LENGTH;

    char *json = response_to_json(&resp);
    check_int("wire.to_json", json != NULL, 1);
    if (!json)
        return;

    llm_response_t *back = response_from_json(json);
    check_int("wire.from_json", back != NULL, 1);
    if (back) {
        check_str("wire.finish_reason", back->finish_reason, LLM_FINISH_LENGTH);
        check_int("wire.truncated", llm_finish_is_truncated(back->finish_reason), 1);
    }
    cJSON_free(json);
    llm_response_free(back);
}

int main(void)
{
    printf("running finish_reason canonical SSoT regression\n");
    test_norm_openai();
    test_norm_anthropic();
    test_norm_google();
    test_norm_passthrough();
    test_norm_missing();
    test_truncated();
    test_wire_roundtrip();

    if (fail_count == 0) {
        printf("All %d finish-reason assertions passed\n", test_count);
        return 0;
    }
    printf("%d/%d finish-reason assertions failed\n", fail_count, test_count);
    return 1;
}
