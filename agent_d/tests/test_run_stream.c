// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file test_run_stream.c
 * @brief LLM 流式解复用直测（white-box include，无 daemon 依赖）：
 *        RS 分帧（T/R/U/E）跨 payload 状态机、真实增量分片、UTF-8 对齐、
 *        大帧动态增长与替换/追加语义。
 */

#include "../src/agent_run_stream.c"

#include <stdio.h>
#include <string.h>

#include "airy_string.h"

static int g_fail = 0;

#define CHECK(cond, name)                \
    do {                                 \
        if (cond) {                      \
            printf("PASS %s\n", (name)); \
        } else {                         \
            printf("FAIL %s\n", (name)); \
            g_fail++;                    \
        }                                \
    } while (0)

/* 增量捕获：拼接所有 delta，并逐片校验「无半个字符」。 */
#define SEEN_MAX 262144

static char g_seen[SEEN_MAX];
static size_t g_seen_len = 0;
static int g_delta_ok = 1;
static size_t g_delta_calls = 0;

static void cap_text(const char *delta, size_t dlen, void *ud)
{
    (void)ud;
    g_delta_calls++;
    if (!string_utf8_validate(delta, dlen))
        g_delta_ok = 0;
    if (g_seen_len + dlen < sizeof(g_seen)) {
        AIRY_MEMCPY(g_seen + g_seen_len, delta, dlen);
        g_seen_len += dlen;
        g_seen[g_seen_len] = '\0';
    }
}

static void cap_reset(void)
{
    g_seen_len = 0;
    g_seen[0] = '\0';
    g_delta_ok = 1;
    g_delta_calls = 0;
}

/* 按固定切片喂入：模拟 recv() 定长切片造成的任意分片边界。 */
static void feed_sliced(agent_stream_t *st, const char *data, size_t len, size_t slice)
{
    size_t i = 0;
    while (i < len) {
        size_t n = (len - i < slice) ? (len - i) : slice;
        agent_stream_feed(st, data + i, n);
        i += n;
    }
}

static size_t mk_frame(char *out, size_t cap, char tag, const char *body)
{
    int n = snprintf(out, cap, "\x1e%c%s\x1e", tag, body);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static void test_plain_text(void)
{
    agent_stream_t st;
    agent_stream_sink_t sink = {cap_text, NULL};
    agent_stream_result_t r;
    const char *msg = "Hello stream world";

    cap_reset();
    agent_stream_init(&st, &sink);
    feed_sliced(&st, msg, strlen(msg), 3);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);

    CHECK(strcmp(g_seen, msg) == 0, "text: deltas concatenate to payload");
    CHECK(strcmp(r.text, msg) == 0, "text: aggregate equals payload");
    CHECK(g_delta_calls > 1, "text: emitted incrementally");
    CHECK(r.reason == NULL && r.tools == NULL, "text: no frames -> no reason/tools");
    CHECK(r.error_code == 0, "text: no error frame -> code 0");

    AIRY_FREE(r.text);
    agent_stream_free(&st);
}

/* 每字节一片：多字节字符必然被切断，增量必须始终是完整 UTF-8 序列。 */
static void test_utf8_split(void)
{
    agent_stream_t st;
    agent_stream_sink_t sink = {cap_text, NULL};
    agent_stream_result_t r;
    const char *msg = "\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x98\x80ok"; /* 中文😀ok */

    cap_reset();
    agent_stream_init(&st, &sink);
    feed_sliced(&st, msg, strlen(msg), 1);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);

    CHECK(strcmp(r.text, msg) == 0, "utf8: aggregate byte-exact");
    CHECK(strcmp(g_seen, msg) == 0, "utf8: delta sum byte-exact");
    CHECK(g_delta_ok, "utf8: every delta is a whole character");
    CHECK(g_delta_calls == 5, "utf8: one emission per character");

    AIRY_FREE(r.text);
    agent_stream_free(&st);
}

static void test_tool_frame(void)
{
    agent_stream_t st;
    agent_stream_sink_t sink = {cap_text, NULL};
    agent_stream_result_t r;
    const char *body = "[{\"id\":\"c1\",\"type\":\"function\","
                       "\"function\":{\"name\":\"fs_read\",\"arguments\":\"{}\"}}]";
    char wire[512];
    size_t wl = mk_frame(wire, sizeof(wire), 'T', body);

    CHECK(wl > 0, "tool: frame built");

    /* 正文 + 工具帧 + 正文，帧体被切成 1 字节片。 */
    cap_reset();
    agent_stream_init(&st, &sink);
    agent_stream_feed(&st, "A", 1);
    feed_sliced(&st, wire, wl, 1);
    agent_stream_feed(&st, "B", 1);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);

    CHECK(strcmp(r.text, "AB") == 0, "tool: frame removed from text");
    CHECK(r.tools && cJSON_GetArraySize(r.tools) == 1, "tool: array parsed");
    if (r.tools) {
        cJSON *fn = cJSON_GetObjectItem(cJSON_GetArrayItem(r.tools, 0), "function");
        cJSON *nm = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
        CHECK(cJSON_IsString(nm) && strcmp(nm->valuestring, "fs_read") == 0, "tool: name intact");
    }

    AIRY_FREE(r.text);
    if (r.tools)
        cJSON_Delete(r.tools);
    agent_stream_free(&st);
}

/* 多轮：后一轮 'T' 帧替换前一轮（替换语义），互不污染。 */
static void test_tool_replace(void)
{
    agent_stream_t st;
    agent_stream_sink_t sink = {cap_text, NULL};
    agent_stream_result_t r;
    const char *b1 = "[{\"id\":\"c1\",\"type\":\"function\","
                     "\"function\":{\"name\":\"fs_read\",\"arguments\":\"{}\"}}]";
    const char *b2 = "[{\"id\":\"c2\",\"type\":\"function\","
                     "\"function\":{\"name\":\"fs_write\",\"arguments\":\"{}\"}}]";
    char w1[512];
    char w2[512];
    size_t l1 = mk_frame(w1, sizeof(w1), 'T', b1);
    size_t l2 = mk_frame(w2, sizeof(w2), 'T', b2);

    cap_reset();
    agent_stream_init(&st, &sink);
    feed_sliced(&st, w1, l1, 7);
    feed_sliced(&st, w2, l2, 5);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);

    CHECK(r.tools && cJSON_GetArraySize(r.tools) == 1, "tool: replace keeps one round");
    if (r.tools) {
        cJSON *fn = cJSON_GetObjectItem(cJSON_GetArrayItem(r.tools, 0), "function");
        cJSON *nm = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
        CHECK(cJSON_IsString(nm) && strcmp(nm->valuestring, "fs_write") == 0,
              "tool: replace keeps the last round");
    }

    AIRY_FREE(r.text);
    if (r.tools)
        cJSON_Delete(r.tools);
    agent_stream_free(&st);
}

/* 思考链逐 delta 下发：必须追加而非替换（替换会塌缩成最后一 token）。 */
static void test_reason_append(void)
{
    agent_stream_t st;
    agent_stream_sink_t sink = {cap_text, NULL};
    agent_stream_result_t r;
    char w1[64];
    char w2[64];
    size_t l1 = mk_frame(w1, sizeof(w1), 'R', "th");
    size_t l2 = mk_frame(w2, sizeof(w2), 'R', "ink");

    cap_reset();
    agent_stream_init(&st, &sink);
    feed_sliced(&st, w1, l1, 3);
    feed_sliced(&st, w2, l2, 3);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);

    CHECK(r.reason && strcmp(r.reason, "think") == 0, "reason: frames appended");
    CHECK(r.text && strcmp(r.text, "") == 0, "reason: empty text is \"\" not NULL");

    AIRY_FREE(r.text);
    AIRY_FREE(r.reason);
    agent_stream_free(&st);
}

static void test_usage_frame(void)
{
    agent_stream_t st;
    agent_stream_sink_t sink = {cap_text, NULL};
    agent_stream_result_t r;
    char w[256];
    size_t l;

    cap_reset();
    agent_stream_init(&st, &sink);
    l = mk_frame(w, sizeof(w), 'U',
                 "{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":77,"
                 "\"cost_usd\":0.25}");
    feed_sliced(&st, w, l, 4);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);
    CHECK(r.tokens == 77, "usage: total_tokens wins");
    CHECK(r.cost == 0.25, "usage: cost parsed");
    AIRY_FREE(r.text);
    agent_stream_free(&st);

    cap_reset();
    agent_stream_init(&st, &sink);
    l = mk_frame(w, sizeof(w), 'U', "{\"prompt_tokens\":10,\"completion_tokens\":5}");
    feed_sliced(&st, w, l, 6);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);
    CHECK(r.tokens == 15, "usage: falls back to prompt+completion when total absent");
    AIRY_FREE(r.text);
    agent_stream_free(&st);
}

/* 'E' 帧：整条流失败，已聚合的正文必须丢弃（不得当可用回复）。 */
static void test_error_frame(void)
{
    agent_stream_t st;
    agent_stream_sink_t sink = {cap_text, NULL};
    agent_stream_result_t r;
    char w[256];
    size_t l = mk_frame(w, sizeof(w), 'E',
                        "{\"error\":{\"code\":-32603,\"message\":\"upstream boom\"}}");

    cap_reset();
    agent_stream_init(&st, &sink);
    agent_stream_feed(&st, "partial", 7);
    feed_sliced(&st, w, l, 5);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);

    CHECK(r.error_code == -32603, "error: code parsed");
    CHECK(strcmp(r.error_msg, "upstream boom") == 0, "error: message parsed");
    CHECK(r.text && strcmp(r.text, "") == 0, "error: partial text discarded");

    AIRY_FREE(r.text);
    agent_stream_free(&st);
}

/* 帧体远超初始 32KB 容量：必须动态增长而不是截断成非法 JSON。 */
static void test_large_frame(void)
{
    agent_stream_t st;
    agent_stream_sink_t sink = {cap_text, NULL};
    agent_stream_result_t r;
    const size_t big = 70000;
    char *args = (char *)AIRY_MALLOC(big + 1);
    char *body = (char *)AIRY_MALLOC(big + 256);
    char *wire = NULL;
    size_t bl = 0;

    CHECK(args && body, "large: scratch allocated");
    if (!args || !body) {
        AIRY_FREE(args);
        AIRY_FREE(body);
        return;
    }
    AIRY_MEMSET(args, 'x', big);
    args[big] = '\0';
    int n = snprintf(body, big + 256,
                     "[{\"id\":\"c9\",\"type\":\"function\",\"function\":"
                     "{\"name\":\"fs_write\",\"arguments\":\"%s\"}}]",
                     args);
    if (n > 0 && (size_t)n < big + 256) {
        bl = (size_t)n;
        wire = (char *)AIRY_MALLOC(bl + 8);
    }
    CHECK(wire != NULL, "large: wire allocated");
    if (wire) {
        size_t wl = mk_frame(wire, bl + 8, 'T', body);
        cap_reset();
        agent_stream_init(&st, &sink);
        feed_sliced(&st, wire, wl, 4096);
        agent_stream_finish(&st);
        agent_stream_take(&st, &r);
        CHECK(r.tools && cJSON_GetArraySize(r.tools) == 1, "large: frame survives growth");
        if (r.tools) {
            cJSON *fn = cJSON_GetObjectItem(cJSON_GetArrayItem(r.tools, 0), "function");
            cJSON *ar = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
            CHECK(cJSON_IsString(ar) && strlen(ar->valuestring) == big,
                  "large: arguments not truncated");
        }
        AIRY_FREE(r.text);
        if (r.tools)
            cJSON_Delete(r.tools);
        agent_stream_free(&st);
        AIRY_FREE(wire);
    }
    AIRY_FREE(args);
    AIRY_FREE(body);
}

/* 孤立 RS（非帧起始）不得吞内容：按正文透出。 */
static void test_stray_rs(void)
{
    agent_stream_t st;
    agent_stream_sink_t sink = {cap_text, NULL};
    agent_stream_result_t r;
    const char wire[] = {'a', '\x1e', 'z'};

    cap_reset();
    agent_stream_init(&st, &sink);
    feed_sliced(&st, wire, sizeof(wire), 2);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);
    CHECK(r.text[0] == 'a' && r.text[1] == '\x1e' && r.text[2] == 'z' && r.text[3] == '\0',
          "stray: RS with non-tag preserved verbatim");
    AIRY_FREE(r.text);
    agent_stream_free(&st);

    /* payload 末尾的 RS 挂起判定，紧接下一 payload 的非 tag 字节 */
    cap_reset();
    agent_stream_init(&st, &sink);
    agent_stream_feed(&st, "a\x1e", 2);
    agent_stream_feed(&st, "z", 1);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);
    CHECK(strcmp(r.text, "a\x1e" "z") == 0, "stray: RS straddling payloads preserved");
    AIRY_FREE(r.text);
    agent_stream_free(&st);

    /* 纯尾部 RS 在流结束时透出 */
    cap_reset();
    agent_stream_init(&st, &sink);
    agent_stream_feed(&st, "x\x1e", 2);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);
    CHECK(strcmp(r.text, "x\x1e") == 0, "stray: trailing RS flushed at end");
    AIRY_FREE(r.text);
    agent_stream_free(&st);
}

/* 无 sink 时只聚合不推送（非流式调用方的安全路径）。 */
static void test_no_sink(void)
{
    agent_stream_t st;
    agent_stream_result_t r;

    cap_reset();
    agent_stream_init(&st, NULL);
    agent_stream_feed(&st, "silent", 6);
    agent_stream_finish(&st);
    agent_stream_take(&st, &r);
    CHECK(g_delta_calls == 0, "sink: absent sink emits nothing");
    CHECK(r.text && strcmp(r.text, "silent") == 0, "sink: aggregate still available");
    AIRY_FREE(r.text);
    agent_stream_free(&st);
}

int main(void)
{
    test_plain_text();
    test_utf8_split();
    test_tool_frame();
    test_tool_replace();
    test_reason_append();
    test_usage_frame();
    test_error_frame();
    test_large_frame();
    test_stray_rs();
    test_no_sink();
    if (g_fail)
        printf("FAILURES: %d\n", g_fail);
    else
        printf("ALL PASS\n");
    return g_fail ? 1 : 0;
}
