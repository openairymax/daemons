// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_ledger.c
 * @brief 上下文台账单元测试（13-semantic-cache-context-ledger.md §6 测试要点）
 *
 * 覆盖：追加、窗口、预算、append-only 状态流转（mark 可回放）、
 * token 一致性（复用 token_standard）、统计、参数校验。
 */

#include "ledger.h"

#include "airy_memory.h"
#include "error.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_create_destroy(void)
{
    printf("  test_create_destroy...\n");
    mem_ledger_t *l = mem_ledger_create(0, 0);
    assert(l != NULL);

    mem_ledger_stats_t st;
    mem_ledger_stats(l, &st);
    assert(st.sessions == 0 && st.entries == 0);

    mem_ledger_destroy(l);
    mem_ledger_destroy(NULL);
    printf("    PASSED\n");
}

static void test_append_and_window(void)
{
    printf("  test_append_and_window...\n");
    mem_ledger_t *l = mem_ledger_create(1024, 0.8);
    assert(l != NULL);

    ledger_entry_in_t in[3] = {
        {.entry_type = LEDGER_ENTRY_SYSTEM, .text = "system prompt", .source = "gateway"},
        {.entry_type = LEDGER_ENTRY_USER, .text = "user message here", .source = "gateway"},
        {.entry_type = LEDGER_ENTRY_ASSISTANT, .text = "assistant reply", .token_out = 42,
         .source = "think"},
    };

    char *ledger_id = NULL;
    int ret = mem_ledger_append(l, "sess-1", in, 3, &ledger_id);
    assert(ret == AIRY_SUCCESS);
    assert(ledger_id != NULL && strlen(ledger_id) == 32);
    AIRY_FREE(ledger_id);

    ledger_window_t win;
    ret = mem_ledger_window(l, "sess-1", &win);
    assert(ret == AIRY_SUCCESS);
    assert(win.count == 3);
    assert(win.total_tokens > 0); /* 由 text 估算 */
    assert(win.warn == 0);        /* 1024*0.8=819.2，估算 token 远低于 */

    /* 顺序保持 append 顺序（旧 → 新） */
    assert(win.entries[0].entry_type == LEDGER_ENTRY_SYSTEM);
    assert(win.entries[1].entry_type == LEDGER_ENTRY_USER);
    assert(win.entries[2].entry_type == LEDGER_ENTRY_ASSISTANT);
    assert(win.entries[2].token_out == 42);
    assert(win.entries[0].status == LEDGER_STATUS_ACTIVE);

    mem_ledger_window_free(&win);

    /* 会话不存在 → 空窗口 */
    ret = mem_ledger_window(l, "no-such-session", &win);
    assert(ret == AIRY_SUCCESS);
    assert(win.count == 0 && win.total_tokens == 0);
    mem_ledger_window_free(&win);

    mem_ledger_destroy(l);
    printf("    PASSED\n");
}

static void test_budget(void)
{
    printf("  test_budget...\n");
    mem_ledger_t *l = mem_ledger_create(50, 0.5); /* 预算 50，告警 25 */
    assert(l != NULL);

    /* 显式 token_in=40，避免估算不确定性 */
    ledger_entry_in_t in = {
        .entry_type = LEDGER_ENTRY_USER,
        .text = "x",
        .token_in = 40,
        .source = "gateway",
    };
    assert(mem_ledger_append(l, "sess-b", &in, 1, NULL) == AIRY_SUCCESS);

    size_t used = 0, limit = 0, headroom = 0;
    assert(mem_ledger_budget(l, "sess-b", &used, &limit, &headroom) == AIRY_SUCCESS);
    assert(used == 40);
    assert(limit == 50);
    assert(headroom == 10);

    ledger_window_t win;
    assert(mem_ledger_window(l, "sess-b", &win) == AIRY_SUCCESS);
    assert(win.warn == 1); /* 40 >= 50*0.5=25 */
    mem_ledger_window_free(&win);

    /* 会话不存在：默认预算、全量 headroom */
    assert(mem_ledger_budget(l, "no-sess", &used, &limit, &headroom) == AIRY_SUCCESS);
    assert(used == 0 && limit == 50 && headroom == 50);

    mem_ledger_destroy(l);
    printf("    PASSED\n");
}

/* append-only：mark 压缩后 history 可完整回放（原始条目 + status 变更记录） */
static void test_append_only_mark(void)
{
    printf("  test_append_only_mark...\n");
    mem_ledger_t *l = mem_ledger_create(0, 0);
    assert(l != NULL);

    ledger_entry_in_t in[2] = {
        {.entry_type = LEDGER_ENTRY_USER, .text = "first user turn", .source = "gateway"},
        {.entry_type = LEDGER_ENTRY_ASSISTANT, .text = "first reply", .source = "think"},
    };
    char *ledger_id = NULL;
    assert(mem_ledger_append(l, "sess-m", in, 2, &ledger_id) == AIRY_SUCCESS);
    assert(ledger_id != NULL);
    AIRY_FREE(ledger_id);

    /* 标记首条为 COMPRESSED */
    ledger_window_t win;
    assert(mem_ledger_window(l, "sess-m", &win) == AIRY_SUCCESS);
    assert(win.count == 2);
    /* 复制 entry_id 到局部缓冲：win 释放后 ids 不得悬垂 */
    char target_id[33];
    memcpy(target_id, win.entries[0].entry_id, sizeof(target_id));
    const char *ids[1] = {target_id};

    size_t updated = 0;
    assert(mem_ledger_mark(l, "sess-m", ids, 1, LEDGER_STATUS_COMPRESSED, &updated) == AIRY_SUCCESS);
    assert(updated == 1);

    /* 压缩后窗口只含 active 条目 */
    mem_ledger_window_free(&win);
    assert(mem_ledger_window(l, "sess-m", &win) == AIRY_SUCCESS);
    assert(win.count == 1);
    assert(win.entries[0].entry_type == LEDGER_ENTRY_ASSISTANT);
    mem_ledger_window_free(&win);

    /* history 可回放：3 条 = 2 原始 + 1 状态变更记录 */
    ledger_entry_view_t *hist = NULL;
    size_t hcount = 0;
    assert(mem_ledger_history(l, "sess-m", 0, &hist, &hcount) == AIRY_SUCCESS);
    assert(hcount == 3);
    int status_records = 0;
    int original_compressed = 0;
    for (size_t i = 0; i < hcount; i++) {
        if (hist[i].ref_id && strcmp(hist[i].ref_id, target_id) == 0)
            status_records++;
        if (hist[i].status == LEDGER_STATUS_COMPRESSED)
            original_compressed++;
    }
    assert(status_records >= 1);   /* 状态变更记录 ref_id 指向原条目 */
    assert(original_compressed >= 1); /* 原条目状态已迁移 */
    mem_ledger_history_free(hist, hcount);

    /* mark 后 active_tokens 重算：压缩条目不再计入预算 */
    size_t used = 0;
    assert(mem_ledger_budget(l, "sess-m", &used, NULL, NULL) == AIRY_SUCCESS);
    size_t win_tokens = 0;
    assert(mem_ledger_window(l, "sess-m", &win) == AIRY_SUCCESS);
    win_tokens = win.total_tokens;
    mem_ledger_window_free(&win);
    assert(used == win_tokens);

    mem_ledger_destroy(l);
    printf("    PASSED\n");
}

/* token 一致性：同一文本重复追加计数一致（token_standard 确定性） */
static void test_token_consistency(void)
{
    printf("  test_token_consistency...\n");
    mem_ledger_t *l = mem_ledger_create(0, 0);
    assert(l != NULL);

    const char *text = "The context ledger counts tokens using the shared standard.";
    ledger_entry_in_t in = {.entry_type = LEDGER_ENTRY_USER, .text = text, .source = "gateway"};

    size_t t1 = 0, t2 = 0;
    for (int i = 0; i < 2; i++) {
        ledger_window_t win;
        assert(mem_ledger_append(l, "sess-t", &in, 1, NULL) == AIRY_SUCCESS);
        assert(mem_ledger_window(l, "sess-t", &win) == AIRY_SUCCESS);
        size_t total = 0;
        for (size_t j = 0; j < win.count; j++)
            total += win.entries[j].token_in;
        if (i == 0)
            t1 = total;
        else
            t2 = total;
        mem_ledger_window_free(&win);
    }
    assert(t1 > 0);
    assert(t2 == t1 * 2); /* 两条相同文本 → 计数恰好两倍（计数确定性） */

    mem_ledger_destroy(l);
    printf("    PASSED\n");
}

static void test_stats_and_validation(void)
{
    printf("  test_stats_and_validation...\n");
    mem_ledger_t *l = mem_ledger_create(0, 0);
    assert(l != NULL);

    ledger_entry_in_t in = {.entry_type = LEDGER_ENTRY_TOOL_DEF, .text = "tool def", .source = "gateway"};
    assert(mem_ledger_append(l, "s1", &in, 1, NULL) == AIRY_SUCCESS);
    assert(mem_ledger_append(l, "s2", &in, 1, NULL) == AIRY_SUCCESS);

    mem_ledger_stats_t st;
    mem_ledger_stats(l, &st);
    assert(st.sessions == 2);
    assert(st.entries == 2);
    assert(st.total_tokens > 0);

    /* 参数校验 */
    assert(mem_ledger_append(NULL, "s", &in, 1, NULL) == AIRY_ERR_INVALID_PARAM);
    assert(mem_ledger_append(l, NULL, &in, 1, NULL) == AIRY_ERR_INVALID_PARAM);
    assert(mem_ledger_append(l, "", &in, 1, NULL) == AIRY_ERR_INVALID_PARAM);
    assert(mem_ledger_window(NULL, "s", NULL) == AIRY_ERR_INVALID_PARAM);
    /* 参数错误优先：NULL entry_ids 且 count>0 → INVALID_PARAM */
    assert(mem_ledger_mark(l, "s1", NULL, 1, LEDGER_STATUS_EVICTED, NULL) == AIRY_ERR_INVALID_PARAM);
    /* 参数合法但会话不存在 → NOT_FOUND */
    const char *nid = "00000000000000000000000000000000";
    assert(mem_ledger_mark(l, "no-sess", &nid, 1, LEDGER_STATUS_EVICTED, NULL) == AIRY_ERR_NOT_FOUND);

    mem_ledger_destroy(l);
    printf("    PASSED\n");
}

/* 容量上限（H6 回归）：会话数超过 max_sessions 拒绝（防 session 洪水 DoS）；
 * count==0 幂等成功且不出 ledger_id。 */
static void test_capacity_limits(void)
{
    printf("  test_capacity_limits...\n");
    mem_ledger_t *l = mem_ledger_create(0, 0);
    assert(l != NULL);

    ledger_entry_in_t in = {.entry_type = LEDGER_ENTRY_USER, .text = "cap", .source = "gateway"};
    /* DEFAULT_MAX_SESSIONS=1024：第 1025 个会话应被拒 */
    int rejected = 0;
    for (int i = 0; i < 1025; i++) {
        char sid[32];
        snprintf(sid, sizeof(sid), "sess-cap-%d", i);
        int ret = mem_ledger_append(l, sid, &in, 1, NULL);
        if (ret != AIRY_SUCCESS) {
            rejected++;
            assert(ret == AIRY_ERR_OVERFLOW);
            break;
        }
    }
    assert(rejected == 1);
    mem_ledger_stats_t st;
    mem_ledger_stats(l, &st);
    assert(st.sessions == 1024);

    /* count==0：成功、无 ledger_id、不产生会话 */
    char *lid = (char *)0x1; /* 哨兵：必须被置 NULL */
    assert(mem_ledger_append(l, "never-created", NULL, 0, &lid) == AIRY_SUCCESS);
    assert(lid == NULL);
    mem_ledger_stats(l, &st);
    assert(st.sessions == 1024);

    mem_ledger_destroy(l);
    printf("    PASSED\n");
}

/* mark 幂等与状态校验（H17/H10 回归）：同状态重复 mark 不再叠加记录；
 * 非法 status 拒绝。 */
static void test_mark_idempotent_and_validate(void)
{
    printf("  test_mark_idempotent_and_validate...\n");
    mem_ledger_t *l = mem_ledger_create(0, 0);
    assert(l != NULL);

    ledger_entry_in_t in = {.entry_type = LEDGER_ENTRY_USER, .text = "x", .source = "gateway"};
    char *lid = NULL;
    assert(mem_ledger_append(l, "sess-idem", &in, 1, &lid) == AIRY_SUCCESS);
    assert(lid != NULL);
    AIRY_FREE(lid);

    ledger_window_t win;
    assert(mem_ledger_window(l, "sess-idem", &win) == AIRY_SUCCESS);
    char target_id[33];
    memcpy(target_id, win.entries[0].entry_id, sizeof(target_id));
    const char *ids[1] = {target_id};
    mem_ledger_window_free(&win);

    size_t updated = 0;
    assert(mem_ledger_mark(l, "sess-idem", ids, 1, LEDGER_STATUS_COMPRESSED, &updated) == AIRY_SUCCESS);
    assert(updated == 1);
    /* 重复 mark 同状态：幂等，updated=0，不追加记录 */
    updated = 0;
    assert(mem_ledger_mark(l, "sess-idem", ids, 1, LEDGER_STATUS_COMPRESSED, &updated) == AIRY_SUCCESS);
    assert(updated == 0);

    ledger_entry_view_t *hist = NULL;
    size_t hcount = 0;
    assert(mem_ledger_history(l, "sess-idem", 0, &hist, &hcount) == AIRY_SUCCESS);
    assert(hcount == 2); /* 1 原始 + 1 状态变更（幂等不叠加） */
    mem_ledger_history_free(hist, hcount);

    /* 非法 status（越界值）拒绝 */
    const char *bad_status[] = {target_id};
    assert(mem_ledger_mark(l, "sess-idem", bad_status, 1, 99, NULL) == AIRY_ERR_INVALID_PARAM);
    assert(mem_ledger_mark(l, "sess-idem", bad_status, 1, -1, NULL) == AIRY_ERR_INVALID_PARAM);

    mem_ledger_destroy(l);
    printf("    PASSED\n");
}

/* 批量 append 原子性（H8 回归）：非法 entry_type 在写入前整体拒绝，
 * 不产生部分成功/残留会话。 */
static void test_append_atomic_reject(void)
{
    printf("  test_append_atomic_reject...\n");
    mem_ledger_t *l = mem_ledger_create(0, 0);
    assert(l != NULL);

    ledger_entry_in_t in[2] = {
        {.entry_type = LEDGER_ENTRY_USER, .text = "ok", .source = "gateway"},
        {.entry_type = 777, .text = "bad type", .source = "gateway"}, /* 越界类型 */
    };
    char *lid = NULL;
    assert(mem_ledger_append(l, "sess-atomic", in, 2, &lid) == AIRY_ERR_INVALID_PARAM);
    assert(lid == NULL);

    /* 不得残留会话或条目 */
    mem_ledger_stats_t st;
    mem_ledger_stats(l, &st);
    assert(st.sessions == 0 && st.entries == 0);
    ledger_window_t win;
    assert(mem_ledger_window(l, "sess-atomic", &win) == AIRY_SUCCESS);
    assert(win.count == 0);
    mem_ledger_window_free(&win);

    mem_ledger_destroy(l);
    printf("    PASSED\n");
}

int main(void)
{
    printf("=== Context Ledger Unit Tests ===\n");
    test_create_destroy();
    test_append_and_window();
    test_budget();
    test_append_only_mark();
    test_token_consistency();
    test_stats_and_validation();
    test_capacity_limits();
    test_mark_idempotent_and_validate();
    test_append_atomic_reject();
    printf("=== All ledger tests PASSED ===\n");
    return 0;
}
