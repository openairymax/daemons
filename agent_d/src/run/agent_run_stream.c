// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_run_stream.c
 * @brief LLM 流式响应解复用（RS 分帧）与真实增量推送。
 *
 * 消费 llm_d 的 complete_stream：裸字节即正文增量，控制帧
 * RS <tag> <body> RS 带内传递 tool_calls（'T'）、reasoning（'R'）、
 * usage（'U'）与错误（'E'）；帧协议权威见 commons/include/airy_llm_stream.h。
 *
 * 与 atoms/coreloopthree 的 llm_svc_adapter_stream.c 同协议不同消费形状：
 * 该侧把帧体回填进完整 llm_response_t，本侧按工具循环的需求吐出「增量正文
 * + cJSON tool_calls + token/cost + 思考链」。两者共享同一分帧语义。
 *
 * 上屏增量按 UTF-8 序列对齐：多字节字符跨 recv 分片时先回撤，凑齐再推；
 * 因此 sum(增量) 恒等于最终正文（逐字节），客户端不会收到半个字符。
 */

#include "agent_run_internal.h"

#include "airy_llm_stream.h"
#include "airy_memory.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

/* 正文/思考链聚合：按需倍增，恒 NUL 结尾。 */
static int run_buf_add(char **buf, size_t *len, size_t *cap, const char *data, size_t add)
{
    if (add == 0)
        return 1;
    if (*len + add + 1 > *cap) {
        size_t cap2 = *cap ? *cap : 4096;
        while (cap2 < *len + add + 1)
            cap2 *= 2;
        char *grown = (char *)AIRY_REALLOC(*buf, cap2);
        if (!grown)
            return 0;
        *buf = grown;
        *cap = cap2;
    }
    AIRY_MEMCPY(*buf + *len, data, add);
    *len += add;
    (*buf)[*len] = '\0';
    return 1;
}

/* 尾部不完整 UTF-8 序列的字节数（0-3）：回撤长度。 */
static size_t run_utf8_tail(const char *s, size_t len)
{
    size_t cont = 0;
    while (cont < 4 && cont < len) {
        unsigned char c = (unsigned char)s[len - 1 - cont];
        if ((c & 0xC0u) == 0x80u) {
            cont++;
            continue;
        }
        size_t need = 1;
        if ((c & 0xE0u) == 0xC0u)
            need = 2;
        else if ((c & 0xF0u) == 0xE0u)
            need = 3;
        else if ((c & 0xF8u) == 0xF0u)
            need = 4;
        return (cont + 1 < need) ? cont + 1 : 0;
    }
    return cont;
}

/* 正文段：聚合 + 推送已对齐的增量前缀（零拷贝，临时 NUL 结尾）。 */
static void run_emit_text(agent_stream_t *st, const char *data, size_t len)
{
    if (len == 0)
        return;
    if (!run_buf_add(&st->text, &st->text_len, &st->text_cap, data, len))
        return;
    size_t safe = st->text_len - run_utf8_tail(st->text, st->text_len);
    if (safe <= st->text_sent)
        return;
    char *delta = st->text + st->text_sent;
    size_t dlen = safe - st->text_sent;
    st->text_sent = safe;
    if (!st->sink.on_text)
        return;
    char saved = st->text[safe];
    st->text[safe] = '\0';
    st->sink.on_text(delta, dlen, st->sink.ud);
    st->text[safe] = saved;
}

/* 'T'：tool_calls JSON 数组，一轮一帧，替换语义。 */
static void run_take_tools(agent_stream_t *st)
{
    cJSON *arr = cJSON_Parse(st->frame);
    if (!arr || !cJSON_IsArray(arr)) {
        SVC_LOG_WARN("agent.stream: tool frame body is not a JSON array (%zu bytes)",
                     st->frame_len);
        if (arr)
            cJSON_Delete(arr);
        return;
    }
    cJSON_Delete(st->tools);
    st->tools = arr;
}

/* 'U'：真实用量与费用（total 缺省回退 prompt+completion）。 */
static void run_take_usage(agent_stream_t *st)
{
    cJSON *u = cJSON_Parse(st->frame);
    if (!u)
        return;
    cJSON *pt = cJSON_GetObjectItem(u, "prompt_tokens");
    cJSON *ct = cJSON_GetObjectItem(u, "completion_tokens");
    cJSON *tt = cJSON_GetObjectItem(u, "total_tokens");
    cJSON *cost = cJSON_GetObjectItem(u, "cost_usd");
    uint64_t total = 0;
    if (cJSON_IsNumber(tt) && tt->valuedouble > 0)
        total = (uint64_t)tt->valuedouble;
    if (total == 0) {
        if (cJSON_IsNumber(pt) && pt->valuedouble > 0)
            total += (uint64_t)pt->valuedouble;
        if (cJSON_IsNumber(ct) && ct->valuedouble > 0)
            total += (uint64_t)ct->valuedouble;
    }
    if (total > 0)
        st->tokens = total;
    if (cJSON_IsNumber(cost))
        st->cost = cost->valuedouble;
    cJSON_Delete(u);
}

/* 'E'：整条流失败。丢弃已聚合的正文/思考/工具，避免调用方把部分内容
 * 当作可用回复（与 coreloopthree 同一语义）。 */
static void run_take_error(agent_stream_t *st)
{
    cJSON *root = cJSON_Parse(st->frame);
    cJSON *err = root ? cJSON_GetObjectItem(root, "error") : NULL;
    cJSON *code = err ? cJSON_GetObjectItem(err, "code") : NULL;
    cJSON *msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
    st->error_code = cJSON_IsNumber(code) ? code->valueint : -32602;
    AIRY_STRNCPY_TERM(st->error_msg,
                      (cJSON_IsString(msg) && msg->valuestring) ? msg->valuestring
                                                                : "LLM stream error",
                      sizeof(st->error_msg));
    if (root)
        cJSON_Delete(root);
    AIRY_FREE(st->text);
    st->text = NULL;
    st->text_len = 0;
    st->text_cap = 0;
    st->text_sent = 0;
    AIRY_FREE(st->reason);
    st->reason = NULL;
    st->reason_len = 0;
    st->reason_cap = 0;
    cJSON_Delete(st->tools);
    st->tools = NULL;
}

/* 帧闭合：按 tag 分派并复位帧状态。 */
static void run_frame_dispatch(agent_stream_t *st)
{
    if (st->frame_dropped > 0) {
        SVC_LOG_WARN("agent.stream: frame tag '%c' dropped %zu bytes (FRAME_MAX exceeded)",
                     st->frame_tag, st->frame_dropped);
    } else if (st->frame && st->frame_len > 0) {
        switch (st->frame_tag) {
        case AIRY_LLM_STREAM_TAG_TOOL:
            run_take_tools(st);
            break;
        case AIRY_LLM_STREAM_TAG_REASON:
            run_buf_add(&st->reason, &st->reason_len, &st->reason_cap, st->frame, st->frame_len);
            break;
        case AIRY_LLM_STREAM_TAG_USAGE:
            run_take_usage(st);
            break;
        case AIRY_LLM_STREAM_TAG_ERROR:
            run_take_error(st);
            break;
        default:
            break;
        }
    }
    st->frame_tag = 0;
    st->frame_len = 0;
    st->frame_dropped = 0;
}

/* 帧体累积直至闭合 RS；返回消耗字节数。跨 payload 的半帧由 frame_tag 保留。 */
static size_t run_frame_drain(agent_stream_t *st, const char *data, size_t len)
{
    size_t i = 0;
    while (i < len && st->frame_tag) {
        unsigned char c = (unsigned char)data[i];
        if (c == AIRY_LLM_STREAM_RS) {
            run_frame_dispatch(st);
        } else if (st->frame_dropped == 0 && st->frame_len + 1 < AIRY_LLM_STREAM_FRAME_MAX) {
            if (st->frame_len + 2 > st->frame_cap) {
                size_t cap = st->frame_cap ? st->frame_cap : AIRY_LLM_STREAM_FRAME_CAP;
                while (cap < st->frame_len + 2)
                    cap *= 2;
                char *grown = (char *)AIRY_REALLOC(st->frame, cap);
                if (!grown) {
                    st->frame_dropped = 1; /* OOM：与超限同路径 fail-closed */
                    st->frame_len = 0;
                    continue;
                }
                st->frame = grown;
                st->frame_cap = cap;
            }
            st->frame[st->frame_len++] = (char)c;
            st->frame[st->frame_len] = '\0';
        } else {
            st->frame_dropped++;
        }
        i++;
    }
    return i;
}

void agent_stream_init(agent_stream_t *st, const agent_stream_sink_t *sink)
{
    if (!st)
        return;
    AIRY_MEMSET(st, 0, sizeof(*st));
    if (sink)
        st->sink = *sink;
}

void agent_stream_free(agent_stream_t *st)
{
    if (!st)
        return;
    AIRY_FREE(st->text);
    AIRY_FREE(st->reason);
    AIRY_FREE(st->frame);
    if (st->tools)
        cJSON_Delete(st->tools);
    AIRY_MEMSET(st, 0, sizeof(*st));
}

void agent_stream_feed(agent_stream_t *st, const char *data, size_t len)
{
    if (!st || !data || len == 0)
        return;

    size_t i = 0;
    if (st->frame_tag) {
        /* 上一 payload 留下的半帧：先补完。 */
        i = run_frame_drain(st, data, len);
        if (st->frame_tag || i >= len)
            return;
    }
    if (st->prev_rs) {
        /* 上一 payload 末尾的 RS：由当前字节判定是否开帧。 */
        st->prev_rs = 0;
        if (AIRY_LLM_STREAM_IS_TAG(data[i])) {
            st->frame_tag = data[i];
            i++;
            if (i >= len)
                return;
            i += run_frame_drain(st, data + i, len - i);
            if (st->frame_tag || i >= len)
                return;
        } else {
            /* 孤立 RS：不是帧起始，按正文透出（内容不丢失）。 */
            run_emit_text(st, "\x1e", 1);
        }
    }

    size_t start = i;
    while (i < len) {
        if ((unsigned char)data[i] != AIRY_LLM_STREAM_RS) {
            i++;
            continue;
        }
        if (i + 1 < len && AIRY_LLM_STREAM_IS_TAG(data[i + 1])) {
            if (i > start)
                run_emit_text(st, data + start, i - start);
            st->frame_tag = data[i + 1];
            i += 2;
            if (i < len) {
                i += run_frame_drain(st, data + i, len - i);
                if (st->frame_tag)
                    return;
            } else {
                return; /* 帧体从下一 payload 开始 */
            }
            start = i;
            continue;
        }
        if (i + 1 >= len) {
            /* payload 末尾的 RS：可能跨 payload 开帧，挂起判定。 */
            if (i > start)
                run_emit_text(st, data + start, i - start);
            st->prev_rs = 1;
            return;
        }
        /* 孤立 RS + 非 tag 字节：两字节按正文透出。 */
        if (i > start)
            run_emit_text(st, data + start, i - start);
        run_emit_text(st, data + i, 2);
        start = i + 2;
        i += 2;
    }
    if (i > start)
        run_emit_text(st, data + start, i - start);
}

void agent_stream_on_chunk(const char *data, size_t len, void *ud)
{
    agent_stream_feed((agent_stream_t *)ud, data, len);
}

void agent_stream_finish(agent_stream_t *st)
{
    if (!st)
        return;
    if (st->frame_tag) {
        /* 流在半帧处结束：帧体残缺，按协议异常整帧丢弃。 */
        SVC_LOG_WARN("agent.stream: truncated '%c' frame at stream end (%zu bytes)",
                     st->frame_tag, st->frame_len);
        st->frame_tag = 0;
        st->frame_len = 0;
        st->frame_dropped = 0;
    }
    if (st->prev_rs) {
        st->prev_rs = 0;
        run_emit_text(st, "\x1e", 1);
    }
    /* 冲刷 UTF-8 回撤段：sum(增量) 与最终正文逐字节一致。 */
    if (st->text && st->text_len > st->text_sent) {
        char *delta = st->text + st->text_sent;
        size_t dlen = st->text_len - st->text_sent;
        if (st->sink.on_text)
            st->sink.on_text(delta, dlen, st->sink.ud);
        st->text_sent = st->text_len;
    }
}

void agent_stream_take(agent_stream_t *st, agent_stream_result_t *out)
{
    if (!st || !out)
        return;
    AIRY_MEMSET(out, 0, sizeof(*out));
    out->text = st->text ? st->text : AIRY_STRDUP("");
    st->text = NULL;
    st->text_len = 0;
    st->text_cap = 0;
    st->text_sent = 0;
    out->reason = st->reason;
    st->reason = NULL;
    st->reason_len = 0;
    st->reason_cap = 0;
    out->tools = st->tools;
    st->tools = NULL;
    out->tokens = st->tokens;
    out->cost = st->cost;
    out->error_code = st->error_code;
    AIRY_STRNCPY_TERM(out->error_msg, st->error_msg, sizeof(out->error_msg));
}
