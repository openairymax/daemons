// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file ledger_handlers.c
 * @brief Context-ledger + prompt-compression RPC handlers.
 *
 * 上下文台账（mem.ledger_*，0.1.5）与提示词压缩（mem.compress，0.1.5）。
 * 依赖全局 g_ledger 实例（创建失败时降级为 no-op）。
 */

#include "ledger_handlers.h"
#include "mem_daemon_ctx.h"

#include "airy_memory.h"
#include "error.h"
#include "daemon_main.h"
#include "ledger.h"
#include "compress.h"

#include <string.h>

/* ── internal helpers ────────────────────────────────────────────────── */

static int ledger_status_from_string(const char *s)
{
    if (strcmp(s, "active") == 0) return LEDGER_STATUS_ACTIVE;
    if (strcmp(s, "evicted") == 0) return LEDGER_STATUS_EVICTED;
    if (strcmp(s, "compressed") == 0) return LEDGER_STATUS_COMPRESSED;
    if (strcmp(s, "deduped") == 0) return LEDGER_STATUS_DEDUPED;
    return -1; /* 未知状态：调用方必须拒绝，防误把 ACTIVE 当目标 */
}

static int ledger_type_from_string(const char *s)
{
    if (strcmp(s, "tool_def") == 0) return LEDGER_ENTRY_TOOL_DEF;
    if (strcmp(s, "user") == 0) return LEDGER_ENTRY_USER;
    if (strcmp(s, "tool_result") == 0) return LEDGER_ENTRY_TOOL_RESULT;
    if (strcmp(s, "assistant") == 0) return LEDGER_ENTRY_ASSISTANT;
    if (strcmp(s, "compressed") == 0) return LEDGER_ENTRY_COMPRESSED;
    if (strcmp(s, "cache_hit") == 0) return LEDGER_ENTRY_CACHE_HIT;
    return LEDGER_ENTRY_SYSTEM;
}

/* ── mem.ledger_append ───────────────────────────────────────────────── */

void handle_ledger_append(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    cJSON *entries = cJSON_GetObjectItem(params, "entries");

    if (!g_ledger || !session || !cJSON_IsString(session) || !entries || !cJSON_IsArray(entries)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "ledger_append 需 session_id + entries[]", id);
        return;
    }

    int n = cJSON_GetArraySize(entries);
    ledger_entry_in_t *in = AIRY_CALLOC(n > 0 ? (size_t)n : 1, sizeof(ledger_entry_in_t));
    if (!in) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "OOM", id);
        return;
    }
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(entries, i);
        cJSON *type = cJSON_GetObjectItem(e, "entry_type");
        cJSON *text = cJSON_GetObjectItem(e, "text");
        cJSON *token_in = cJSON_GetObjectItem(e, "token_in");
        cJSON *token_out = cJSON_GetObjectItem(e, "token_out");
        cJSON *source = cJSON_GetObjectItem(e, "source");
        cJSON *ref_id = cJSON_GetObjectItem(e, "ref_id");
        in[i].entry_type = type && cJSON_IsString(type)
                               ? ledger_type_from_string(type->valuestring)
                               : LEDGER_ENTRY_SYSTEM;
        in[i].text = text && cJSON_IsString(text) ? text->valuestring : NULL;
        in[i].token_in = token_in && cJSON_IsNumber(token_in) && token_in->valuedouble > 0
                             ? (size_t)token_in->valuedouble
                             : 0;
        in[i].token_out = token_out && cJSON_IsNumber(token_out) && token_out->valuedouble > 0
                              ? (size_t)token_out->valuedouble
                              : 0;
        in[i].source = source && cJSON_IsString(source) ? source->valuestring : NULL;
        in[i].ref_id = ref_id && cJSON_IsString(ref_id) ? ref_id->valuestring : NULL;
    }

    char *ledger_id = NULL;
    int ret = mem_ledger_append(g_ledger, session->valuestring, in, (size_t)n, &ledger_id);
    AIRY_FREE(in);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "ledger_append 失败", id);
        AIRY_FREE(ledger_id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "ledger_id", ledger_id ? ledger_id : "");
    cJSON_AddNumberToObject(result, "appended", (double)n);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    AIRY_FREE(ledger_id);
}

/* ── mem.ledger_window ───────────────────────────────────────────────── */

void handle_ledger_window(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    if (!g_ledger || !session || !cJSON_IsString(session)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "ledger_window 需 session_id", id);
        return;
    }
    ledger_window_t win;
    int ret = mem_ledger_window(g_ledger, session->valuestring, &win);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "ledger_window 失败", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < win.count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "entry_id", win.entries[i].entry_id);
        cJSON_AddNumberToObject(item, "seq", (double)win.entries[i].seq);
        cJSON_AddNumberToObject(item, "token_in", (double)win.entries[i].token_in);
        cJSON_AddNumberToObject(item, "token_out", (double)win.entries[i].token_out);
        cJSON_AddStringToObject(item, "source", win.entries[i].source ? win.entries[i].source : "");
        cJSON_AddStringToObject(item, "ref_id", win.entries[i].ref_id ? win.entries[i].ref_id : "");
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(result, "entries", arr);
    cJSON_AddNumberToObject(result, "total_tokens", (double)win.total_tokens);
    cJSON_AddBoolToObject(result, "warn", win.warn ? 1 : 0);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    mem_ledger_window_free(&win);
}

/* ── mem.ledger_budget ───────────────────────────────────────────────── */

void handle_ledger_budget(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    if (!g_ledger || !session || !cJSON_IsString(session)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "ledger_budget 需 session_id", id);
        return;
    }
    size_t used = 0, limit = 0, headroom = 0;
    mem_ledger_budget(g_ledger, session->valuestring, &used, &limit, &headroom);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "used", (double)used);
    cJSON_AddNumberToObject(result, "limit", (double)limit);
    cJSON_AddNumberToObject(result, "headroom", (double)headroom);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ── mem.ledger_mark ─────────────────────────────────────────────────── */

void handle_ledger_mark(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    cJSON *entry_ids = cJSON_GetObjectItem(params, "entry_ids");
    cJSON *status = cJSON_GetObjectItem(params, "status");
    if (!g_ledger || !session || !cJSON_IsString(session) || !entry_ids || !cJSON_IsArray(entry_ids) ||
        !status || !cJSON_IsString(status)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "ledger_mark 需 session_id + entry_ids[] + status", id);
        return;
    }
    int n = cJSON_GetArraySize(entry_ids);
    const char **ids = AIRY_CALLOC(n > 0 ? (size_t)n : 1, sizeof(char *));
    if (!ids) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "OOM", id);
        return;
    }
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(entry_ids, i);
        ids[i] = cJSON_IsString(e) ? e->valuestring : "";
    }
    size_t updated = 0;
    int st = ledger_status_from_string(status->valuestring);
    if (st < 0) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "ledger_mark 的 status 非法（active|evicted|compressed|deduped）", id);
        AIRY_FREE(ids);
        return;
    }
    int ret = mem_ledger_mark(g_ledger, session->valuestring, ids, (size_t)n, st, &updated);
    AIRY_FREE(ids);
    if (ret != AIRY_SUCCESS && ret != AIRY_ERR_NOT_FOUND) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "ledger_mark 失败", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "updated", (double)updated);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ── mem.ledger_history ──────────────────────────────────────────────── */

void handle_ledger_history(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    cJSON *limit = cJSON_GetObjectItem(params, "limit");
    if (!g_ledger || !session || !cJSON_IsString(session)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "ledger_history 需 session_id", id);
        return;
    }
    ledger_entry_view_t *items = NULL;
    size_t count = 0;
    size_t lim = limit && cJSON_IsNumber(limit) ? clamp_u32(limit->valueint, 0, 10000) : 0;
    int ret = mem_ledger_history(g_ledger, session->valuestring, lim, &items, &count);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "ledger_history 失败", id);
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON *events = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "entry_id", items[i].entry_id);
        cJSON_AddNumberToObject(item, "seq", (double)items[i].seq);
        cJSON_AddNumberToObject(item, "entry_type", (double)items[i].entry_type);
        cJSON_AddNumberToObject(item, "token_in", (double)items[i].token_in);
        cJSON_AddNumberToObject(item, "token_out", (double)items[i].token_out);
        cJSON_AddNumberToObject(item, "status", (double)items[i].status);
        cJSON_AddStringToObject(item, "source", items[i].source ? items[i].source : "");
        cJSON_AddStringToObject(item, "ref_id", items[i].ref_id ? items[i].ref_id : "");
        cJSON_AddItemToArray(events, item);
    }
    cJSON_AddItemToObject(result, "events", events);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    mem_ledger_history_free(items, count);
}

/* ── mem.ledger_stats ────────────────────────────────────────────────── */

void handle_ledger_stats(int id, airy_sock_t client_fd)
{
    if (!g_ledger) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "ledger 未初始化", id);
        return;
    }
    mem_ledger_stats_t st;
    mem_ledger_stats(g_ledger, &st);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "sessions", (double)st.sessions);
    cJSON_AddNumberToObject(result, "entries", (double)st.entries);
    cJSON_AddNumberToObject(result, "total_tokens", (double)st.total_tokens);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

/* ── mem.compress ────────────────────────────────────────────────────── */

/* 提示词压缩（14-prompt-compression.md §3：L1+L2 默认开）。
 * 入参：{session_id, entries:[{entry_id, entry_type, text}]}
 * 返回：{context, saved_tokens, actions:[{entry_id, entry_type, action}], marked}
 * 联动：对压缩条目 ledger.mark(COMPRESSED)，追加 compressed 块条目（可回放）。 */
void handle_compress(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *session = cJSON_GetObjectItem(params, "session_id");
    cJSON *entries = cJSON_GetObjectItem(params, "entries");

    if (!g_ledger || !session || !cJSON_IsString(session) || !entries || !cJSON_IsArray(entries)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "compress 需 session_id + entries[]", id);
        return;
    }

    int n = cJSON_GetArraySize(entries);
    compress_entry_in_t *in = AIRY_CALLOC(n > 0 ? (size_t)n : 1, sizeof(compress_entry_in_t));
    if (!in) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "OOM", id);
        return;
    }
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(entries, i);
        cJSON *eid = cJSON_GetObjectItem(e, "entry_id");
        cJSON *etype = cJSON_GetObjectItem(e, "entry_type");
        cJSON *text = cJSON_GetObjectItem(e, "text");
        in[i].entry_id = eid && cJSON_IsString(eid) ? eid->valuestring : "";
        in[i].entry_type = etype && cJSON_IsString(etype)
                               ? ledger_type_from_string(etype->valuestring)
                               : LEDGER_ENTRY_SYSTEM;
        in[i].text = text && cJSON_IsString(text) ? text->valuestring : NULL;
        in[i].token_in = 0; /* 由 plan 用 token_standard 估算 */
    }

    char *ctx = NULL;
    size_t saved = 0;
    compress_plan_item_t *actions = NULL;
    size_t action_count = 0;
    int ret = mem_compress_plan(g_ledger, session->valuestring, in, (size_t)n, NULL, &ctx, &saved,
                                &actions, &action_count);
    AIRY_FREE(in);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "compress 失败", id);
        mem_compress_plan_free(ctx, actions, action_count);
        return;
    }

    /* 台账联动：按 action 映射状态（DROP→EVICTED / DEDUP→DEDUPED /
     * TRUNCATE|EXTRACT→COMPRESSED）。仅当有实际状态迁移（marked>0）才追加
     * 压缩块：防重复压缩已标记条目时 budget 不降反升（每次叠加新块）。 */
    size_t marked = 0;
    if (action_count > 0) {
        for (size_t i = 0; i < action_count; i++) {
            int st = LEDGER_STATUS_COMPRESSED;
            if (actions[i].action == COMPRESS_ACTION_DROP)
                st = LEDGER_STATUS_EVICTED;
            else if (actions[i].action == COMPRESS_ACTION_DEDUP)
                st = LEDGER_STATUS_DEDUPED;
            const char *one = actions[i].entry_id;
            mem_ledger_mark(g_ledger, session->valuestring, &one, 1, st, &marked);
        }
        if (marked > 0 && ctx && ctx[0]) {
            ledger_entry_in_t block = {0};
            block.entry_type = LEDGER_ENTRY_COMPRESSED;
            block.text = ctx;
            block.source = "ledger";
            block.ref_id = actions[0].entry_id;
            mem_ledger_append(g_ledger, session->valuestring, &block, 1, NULL);
        }
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "context", ctx ? ctx : "");
    cJSON_AddNumberToObject(result, "saved_tokens", (double)saved);
    cJSON_AddNumberToObject(result, "marked", (double)marked);
    cJSON *acts = cJSON_CreateArray();
    for (size_t i = 0; i < action_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "entry_id", actions[i].entry_id);
        cJSON_AddNumberToObject(item, "entry_type", (double)actions[i].entry_type);
        cJSON_AddNumberToObject(item, "action", (double)actions[i].action);
        cJSON_AddItemToArray(acts, item);
    }
    cJSON_AddItemToObject(result, "actions", acts);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    mem_compress_plan_free(ctx, actions, action_count);
}
