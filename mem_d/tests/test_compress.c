// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_compress.c
 * @brief 提示词压缩单元测试（14-prompt-compression.md §6 测试要点）
 *
 * 覆盖：L1 确定性、保护规则（system/tool_def/当前请求）、超长 tool_result
 * 截断、精确去重、超轮数丢弃、L2 抽取触发与预算收敛、台账联动回放。
 */

#include "compress.h"
#include "ledger.h"

#include "airy_memory.h"
#include "error.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int find_action(const compress_plan_item_t *acts, size_t n, int action)
{
    for (size_t i = 0; i < n; i++) {
        if (acts[i].action == action)
            return 1;
    }
    return 0;
}

static int find_entry_action(const compress_plan_item_t *acts, size_t n, const char *entry_id)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(acts[i].entry_id, entry_id) == 0)
            return acts[i].action;
    }
    return COMPRESS_ACTION_NONE;
}

/* 小窗口（预算未超）：不压缩，上下文保留全部条目 */
static void test_short_window_noop(void)
{
    printf("  test_short_window_noop...\n");
    mem_ledger_t *l = mem_ledger_create(0, 0);
    assert(l != NULL);

    compress_entry_in_t entries[3] = {
        {.entry_id = "e1", .entry_type = LEDGER_ENTRY_SYSTEM, .text = "you are a helpful assistant"},
        {.entry_id = "e2", .entry_type = LEDGER_ENTRY_USER, .text = "hello"},
        {.entry_id = "e3", .entry_type = LEDGER_ENTRY_ASSISTANT, .text = "hi there"},
    };

    char *ctx = NULL;
    size_t saved = 0;
    compress_plan_item_t *acts = NULL;
    size_t n = 0;
    assert(mem_compress_plan(l, "sess", entries, 3, NULL, &ctx, &saved, &acts, &n) == AIRY_SUCCESS);
    assert(n == 0); /* 无压缩动作 */
    assert(ctx != NULL);
    assert(strstr(ctx, "helpful assistant") != NULL);
    assert(strstr(ctx, "hello") != NULL);
    assert(strstr(ctx, "hi there") != NULL);

    mem_compress_plan_free(ctx, acts, n);
    mem_ledger_destroy(l);
    printf("    PASSED\n");
}

/* L1 确定性 + tool_result 超长截断：两次压缩结果一致，含 truncated 标记 */
static void test_truncate_determinism(void)
{
    printf("  test_truncate_determinism...\n");
    char tool_out[512];
    snprintf(tool_out, sizeof(tool_out),
             "line one of a very long tool output\nline two\nline three\nline four\nline five\n"
             "line six\nline seven\nline eight\nline nine\nline ten end");

    compress_entry_in_t entries[3] = {
        {.entry_id = "t0", .entry_type = LEDGER_ENTRY_SYSTEM, .text = "system prompt"},
        {.entry_id = "t1", .entry_type = LEDGER_ENTRY_USER, .text = "run the tool please"},
        {.entry_id = "t2", .entry_type = LEDGER_ENTRY_TOOL_RESULT, .text = tool_out,
         .token_in = 5000}, /* 显式超 max_tool_tokens=1024 */
    };

    char *ctx1 = NULL, *ctx2 = NULL;
    size_t s1 = 0, s2 = 0;
    compress_plan_item_t *a1 = NULL, *a2 = NULL;
    size_t n1 = 0, n2 = 0;
    assert(mem_compress_plan(NULL, NULL, entries, 3, NULL, &ctx1, &s1, &a1, &n1) == AIRY_SUCCESS);
    assert(mem_compress_plan(NULL, NULL, entries, 3, NULL, &ctx2, &s2, &a2, &n2) == AIRY_SUCCESS);

    /* L1 确定性：两次结果逐字节一致 */
    assert(n1 == n2);
    assert(strcmp(ctx1, ctx2) == 0);
    assert(s1 == s2);

    /* 截断动作存在且文本含占位符 */
    assert(find_action(a1, n1, COMPRESS_ACTION_TRUNCATE));
    assert(strstr(ctx1, "truncated") != NULL);
    assert(s1 > 0);
    assert(s2 == s1);

    mem_compress_plan_free(ctx1, a1, n1);
    mem_compress_plan_free(ctx2, a2, n2);
    printf("    PASSED\n");
}

/* 保护规则：system / tool_def / 当前请求（最后一条 user）永不压缩 */
static void test_protected_entries(void)
{
    printf("  test_protected_entries...\n");
    /* 构造：system + tool_def + 超长 tool_result（可截断）+ 最后 user（当前请求） */
    char tool_out[512];
    snprintf(tool_out, sizeof(tool_out),
             "a b c d e f g h i j\nk l m n o p q r s t\nu v w x y z\n1 2 3 4 5 6 7 8 9 0\n"
             "end of tool output here");

    compress_entry_in_t entries[4] = {
        {.entry_id = "p0", .entry_type = LEDGER_ENTRY_SYSTEM, .text = "system: follow the rules"},
        {.entry_id = "p1", .entry_type = LEDGER_ENTRY_TOOL_DEF, .text = "tool schema def"},
        {.entry_id = "p2", .entry_type = LEDGER_ENTRY_TOOL_RESULT, .text = tool_out,
         .token_in = 3000},
        {.entry_id = "p3", .entry_type = LEDGER_ENTRY_USER, .text = "summarize the result please"},
    };

    char *ctx = NULL;
    size_t saved = 0;
    compress_plan_item_t *acts = NULL;
    size_t n = 0;
    assert(mem_compress_plan(NULL, NULL, entries, 4, NULL, &ctx, &saved, &acts, &n) == AIRY_SUCCESS);

    /* tool_result 被截断 */
    assert(find_entry_action(acts, n, "p2") == COMPRESS_ACTION_TRUNCATE);
    /* 保护条目无动作 */
    assert(find_entry_action(acts, n, "p0") == COMPRESS_ACTION_NONE);
    assert(find_entry_action(acts, n, "p1") == COMPRESS_ACTION_NONE);
    assert(find_entry_action(acts, n, "p3") == COMPRESS_ACTION_NONE);
    /* 保护条目原文在上下文中完整保留 */
    assert(strstr(ctx, "follow the rules") != NULL);
    assert(strstr(ctx, "tool schema def") != NULL);
    assert(strstr(ctx, "summarize the result please") != NULL);

    mem_compress_plan_free(ctx, acts, n);
    printf("    PASSED\n");
}

/* 精确去重：重复文本保留首条，第二条 DEDUP */
static void test_dedup(void)
{
    printf("  test_dedup...\n");
    compress_entry_in_t entries[3] = {
        {.entry_id = "d1", .entry_type = LEDGER_ENTRY_ASSISTANT, .text = "the answer is 42"},
        {.entry_id = "d2", .entry_type = LEDGER_ENTRY_USER, .text = "what is the answer"},
        {.entry_id = "d3", .entry_type = LEDGER_ENTRY_ASSISTANT, .text = "the answer is 42"},
    };

    char *ctx = NULL;
    size_t saved = 0;
    compress_plan_item_t *acts = NULL;
    size_t n = 0;
    assert(mem_compress_plan(NULL, NULL, entries, 3, NULL, &ctx, &saved, &acts, &n) == AIRY_SUCCESS);

    assert(find_entry_action(acts, n, "d1") == COMPRESS_ACTION_NONE); /* 首条保留 */
    assert(find_entry_action(acts, n, "d3") == COMPRESS_ACTION_DEDUP); /* 重复删除 */
    assert(saved > 0);

    mem_compress_plan_free(ctx, acts, n);
    printf("    PASSED\n");
}

/* 超轮数丢弃：max_turns=2、3 轮会话 → 丢弃第 2 轮，保留首轮与末轮 */
static void test_max_turns_drop(void)
{
    printf("  test_max_turns_drop...\n");
    compress_config_t cfg = {
        .max_tool_tokens = 1024,
        .max_turns = 2,
        .l1_enabled = 1,
        .l2_enabled = 1,
        .dedup = 1,
    };
    compress_entry_in_t entries[6] = {
        {.entry_id = "m1", .entry_type = LEDGER_ENTRY_USER, .text = "first question"},
        {.entry_id = "m2", .entry_type = LEDGER_ENTRY_ASSISTANT, .text = "first answer"},
        {.entry_id = "m3", .entry_type = LEDGER_ENTRY_USER, .text = "second question"},
        {.entry_id = "m4", .entry_type = LEDGER_ENTRY_ASSISTANT, .text = "second answer"},
        {.entry_id = "m5", .entry_type = LEDGER_ENTRY_USER, .text = "third question"},
        {.entry_id = "m6", .entry_type = LEDGER_ENTRY_ASSISTANT, .text = "third answer"},
    };

    char *ctx = NULL;
    size_t saved = 0;
    compress_plan_item_t *acts = NULL;
    size_t n = 0;
    assert(mem_compress_plan(NULL, NULL, entries, 6, &cfg, &ctx, &saved, &acts, &n) == AIRY_SUCCESS);

    /* 第 2 轮（m3/m4）被丢弃 */
    assert(find_entry_action(acts, n, "m3") == COMPRESS_ACTION_DROP);
    assert(find_entry_action(acts, n, "m4") == COMPRESS_ACTION_DROP);
    /* 首轮与末轮保留 */
    assert(find_entry_action(acts, n, "m1") == COMPRESS_ACTION_NONE);
    assert(find_entry_action(acts, n, "m2") == COMPRESS_ACTION_NONE);
    assert(find_entry_action(acts, n, "m5") == COMPRESS_ACTION_NONE);
    assert(find_entry_action(acts, n, "m6") == COMPRESS_ACTION_NONE);
    assert(saved > 0);
    /* 上下文不含被丢弃轮内容 */
    assert(strstr(ctx, "second question") == NULL);
    assert(strstr(ctx, "third question") != NULL);

    mem_compress_plan_free(ctx, acts, n);
    printf("    PASSED\n");
}

/* L2 触发 + 预算收敛：小预算 + 长历史消息 → 抽取摘要，压缩后 token 下降 */
static void test_l2_extract_and_budget(void)
{
    printf("  test_l2_extract_and_budget...\n");
    mem_ledger_t *l = mem_ledger_create(20, 0.8); /* 预算 20，warn_at=16 */
    assert(l != NULL);

    /* 多条长历史消息（>32 tokens），触发 L2 抽取 */
    char msg[600];
    snprintf(msg, sizeof(msg),
             "First we need to analyze the requirement and understand the constraints. "
             "The system must ensure data safety and verify every step carefully. "
             "We should document the design decisions and confirm the final result. "
             "Please review the implementation and make sure nothing is missing. "
             "The summary should include the conclusion and the open questions.");

    ledger_entry_in_t app[3] = {
        {.entry_type = LEDGER_ENTRY_SYSTEM, .text = "system prompt"},
        {.entry_type = LEDGER_ENTRY_USER, .text = "first user message"},
        {.entry_type = LEDGER_ENTRY_ASSISTANT, .text = msg},
    };
    assert(mem_ledger_append(l, "sess-l2", app, 3, NULL) == AIRY_SUCCESS);

    /* 追加一条作为当前请求（最后 user） */
    ledger_entry_in_t cur = {.entry_type = LEDGER_ENTRY_USER, .text = "continue please"};
    assert(mem_ledger_append(l, "sess-l2", &cur, 1, NULL) == AIRY_SUCCESS);

    /* 从台账取窗口（含预算依据），文本由测试提供 */
    ledger_window_t win;
    assert(mem_ledger_window(l, "sess-l2", &win) == AIRY_SUCCESS);
    assert(win.count == 4);

    compress_entry_in_t entries[4];
    for (size_t i = 0; i < 4; i++) {
        entries[i].entry_id = win.entries[i].entry_id;
        entries[i].entry_type = win.entries[i].entry_type;
        /* 文本重建：测试中直接按类型提供原文 */
        switch (entries[i].entry_type) {
        case LEDGER_ENTRY_SYSTEM: entries[i].text = "system prompt"; break;
        case LEDGER_ENTRY_USER:
            entries[i].text = (i == 3) ? "continue please" : "first user message";
            break;
        default: entries[i].text = msg; break;
        }
        entries[i].token_in = 0;
    }

    char *ctx = NULL;
    size_t saved = 0;
    compress_plan_item_t *acts = NULL;
    size_t n = 0;
    assert(mem_compress_plan(l, "sess-l2", entries, 4, NULL, &ctx, &saved, &acts, &n) == AIRY_SUCCESS);

    /* 预算收敛：L2 抽取生效且 saved > 0 */
    assert(saved > 0);
    assert(find_action(acts, n, COMPRESS_ACTION_EXTRACT));
    assert(ctx != NULL);

    /* 保护：system 与当前请求（最后 user）完整保留 */
    assert(strstr(ctx, "system prompt") != NULL);
    assert(strstr(ctx, "continue please") != NULL);

    /* 回退可回放：台账 mark(COMPRESSED) 后 history 含状态变更记录 */
    const char **ids = AIRY_CALLOC(n, sizeof(char *));
    for (size_t i = 0; i < n; i++)
        ids[i] = acts[i].entry_id;
    size_t updated = 0;
    if (n > 0)
        assert(mem_ledger_mark(l, "sess-l2", ids, n, LEDGER_STATUS_COMPRESSED, &updated) == AIRY_SUCCESS);
    AIRY_FREE(ids);

    ledger_entry_view_t *hist = NULL;
    size_t hcount = 0;
    assert(mem_ledger_history(l, "sess-l2", 0, &hist, &hcount) == AIRY_SUCCESS);
    int compressed_records = 0;
    for (size_t i = 0; i < hcount; i++) {
        if (hist[i].status == LEDGER_STATUS_COMPRESSED)
            compressed_records++;
    }
    assert(compressed_records >= (int)n);
    mem_ledger_history_free(hist, hcount);
    mem_ledger_window_free(&win);
    mem_compress_plan_free(ctx, acts, n);

    mem_ledger_destroy(l);
    printf("    PASSED\n");
}

int main(void)
{
    printf("=== Prompt Compression Unit Tests ===\n");
    test_short_window_noop();
    test_truncate_determinism();
    test_protected_entries();
    test_dedup();
    test_max_turns_drop();
    test_l2_extract_and_budget();
    printf("=== All compress tests PASSED ===\n");
    return 0;
}
