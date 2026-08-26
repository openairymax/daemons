/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file ledger.c
 * @brief 上下文台账实现（见 ledger.h）。
 *
 * 结构：会话链表 → 每条目 append-only 单向链。Token 计数复用
 * commons token_standard（airy_token_counter），C 侧单一权威。
 */

#include "ledger.h"
#include "airy_memory.h"
#include "token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LEDGER_ENTRY_ID_HEX 32
#define DEFAULT_BUDGET 32768UL
#define DEFAULT_WARN_RATIO 0.8
#define MAX_SESSION_ID_LEN 255

/* ─── 内部结构 ─────────────────────────────────────────────────────────── */

typedef struct ledger_entry {
    uint64_t seq;
    char entry_id[LEDGER_ENTRY_ID_HEX + 1];
    char *session_id;
    int entry_type;
    size_t token_in;
    size_t token_out;
    char *source;
    int status;
    uint64_t created_at;
    char *ref_id;
    struct ledger_entry *next;
} ledger_entry_t;

typedef struct ledger_session {
    char *session_id;
    ledger_entry_t *head;   /* append-only 链（旧 → 新） */
    ledger_entry_t *tail;
    size_t entry_count;
    size_t active_tokens;   /* active 条目 token_in 合计（预算依据） */
    size_t budget;
    struct ledger_session *next;
} ledger_session_t;

struct mem_ledger {
    ledger_session_t *sessions;
    size_t session_count;
    size_t entry_count;
    size_t total_tokens;
    size_t default_budget;
    double warn_ratio;
    airy_token_counter_t *counter;
    unsigned long seq;
};

/* ─── 工具 ─────────────────────────────────────────────────────────────── */

static uint64_t ledger_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void entry_id_gen(mem_ledger_t *ledger, char out[LEDGER_ENTRY_ID_HEX + 1])
{
    uint64_t now = ledger_now_ns();
    uint64_t r = (uint64_t)rand() ^ (uint64_t)(uintptr_t)ledger;
    snprintf(out, LEDGER_ENTRY_ID_HEX + 1, "%08x%08x%08x%08x",
             (uint32_t)(now & 0xFFFFFFFFUL), (uint32_t)((now >> 32) & 0xFFFFFFFFUL),
             (uint32_t)(r & 0xFFFFFFFFUL), (uint32_t)((unsigned long)++ledger->seq & 0xFFFFFFFFUL));
}

static ledger_session_t *session_find(mem_ledger_t *ledger, const char *session_id)
{
    for (ledger_session_t *s = ledger->sessions; s; s = s->next) {
        if (strcmp(s->session_id, session_id) == 0)
            return s;
    }
    return NULL;
}

/* 计算 active 条目 token 合计（预算依据：status == ACTIVE） */
static void session_recalc_active(ledger_session_t *s)
{
    size_t total = 0;
    for (ledger_entry_t *e = s->head; e; e = e->next) {
        if (e->status == LEDGER_STATUS_ACTIVE)
            total += e->token_in;
    }
    s->active_tokens = total;
}

static ledger_entry_t *entry_find_by_id(ledger_session_t *s, const char *entry_id)
{
    for (ledger_entry_t *e = s->head; e; e = e->next) {
        if (strcmp(e->entry_id, entry_id) == 0)
            return e;
    }
    return NULL;
}

static ledger_entry_view_t *view_from_entry(const ledger_entry_t *e)
{
    ledger_entry_view_t *v = AIRY_CALLOC(1, sizeof(ledger_entry_view_t));
    if (!v)
        return NULL;
    v->seq = e->seq;
    AIRY_STRNCPY_TERM(v->entry_id, e->entry_id, LEDGER_ENTRY_ID_HEX + 1);
    v->session_id = AIRY_STRDUP(e->session_id);
    v->entry_type = e->entry_type;
    v->token_in = e->token_in;
    v->token_out = e->token_out;
    v->source = e->source ? AIRY_STRDUP(e->source) : NULL;
    v->status = e->status;
    v->created_at = e->created_at;
    v->ref_id = e->ref_id ? AIRY_STRDUP(e->ref_id) : NULL;
    return v;
}

/* ─── 生命周期 ─────────────────────────────────────────────────────────── */

mem_ledger_t *mem_ledger_create(size_t default_budget, double warn_ratio)
{
    mem_ledger_t *ledger = AIRY_CALLOC(1, sizeof(mem_ledger_t));
    if (!ledger)
        return NULL;
    ledger->default_budget = default_budget > 0 ? default_budget : DEFAULT_BUDGET;
    ledger->warn_ratio = warn_ratio > 0.0 && warn_ratio <= 1.0 ? warn_ratio : DEFAULT_WARN_RATIO;
    ledger->counter = airy_token_counter_create("gpt-4");
    if (!ledger->counter) {
        AIRY_FREE(ledger);
        return NULL;
    }
    return ledger;
}

void mem_ledger_destroy(mem_ledger_t *ledger)
{
    if (!ledger)
        return;
    ledger_session_t *s = ledger->sessions;
    while (s) {
        ledger_session_t *ns = s->next;
        ledger_entry_t *e = s->head;
        while (e) {
            ledger_entry_t *ne = e->next;
            AIRY_FREE(e->session_id);
            AIRY_FREE(e->source);
            AIRY_FREE(e->ref_id);
            AIRY_FREE(e);
            e = ne;
        }
        AIRY_FREE(s->session_id);
        AIRY_FREE(s);
        s = ns;
    }
    if (ledger->counter)
        airy_token_counter_destroy(ledger->counter);
    AIRY_FREE(ledger);
}

/* ─── 追加（append-only） ─────────────────────────────────────────────── */

int mem_ledger_append(mem_ledger_t *ledger, const char *session_id,
                      const ledger_entry_in_t *entries, size_t count,
                      char **out_ledger_id)
{
    ledger_session_t *s;
    char first_id[LEDGER_ENTRY_ID_HEX + 1] = {0};
    size_t len;

    if (!ledger || !session_id || (!entries && count > 0))
        return AIRY_ERR_INVALID_PARAM;
    len = strlen(session_id);
    if (len == 0 || len > MAX_SESSION_ID_LEN)
        return AIRY_ERR_INVALID_PARAM;

    s = session_find(ledger, session_id);
    if (!s) {
        s = AIRY_CALLOC(1, sizeof(ledger_session_t));
        if (!s)
            return AIRY_ERR_OUT_OF_MEMORY;
        s->session_id = AIRY_STRDUP(session_id);
        s->budget = ledger->default_budget;
        if (!s->session_id) {
            AIRY_FREE(s);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        s->next = ledger->sessions;
        ledger->sessions = s;
        ledger->session_count++;
    }

    for (size_t i = 0; i < count; i++) {
        const ledger_entry_in_t *in = &entries[i];
        ledger_entry_t *e = AIRY_CALLOC(1, sizeof(ledger_entry_t));
        if (!e)
            return AIRY_ERR_OUT_OF_MEMORY;
        entry_id_gen(ledger, e->entry_id);
        if (i == 0)
            AIRY_STRNCPY_TERM(first_id, e->entry_id, LEDGER_ENTRY_ID_HEX + 1);
        e->session_id = AIRY_STRDUP(session_id);
        e->entry_type = in->entry_type;
        e->token_in = in->token_in > 0
                          ? in->token_in
                          : (in->text ? airy_token_counter_count(ledger->counter, in->text) : 0);
        e->token_out = in->token_out;
        e->source = in->source ? AIRY_STRDUP(in->source) : NULL;
        e->status = LEDGER_STATUS_ACTIVE;
        e->created_at = ledger_now_ns();
        e->ref_id = in->ref_id ? AIRY_STRDUP(in->ref_id) : NULL;
        e->seq = ++ledger->seq;

        if (s->tail)
            s->tail->next = e;
        else
            s->head = e;
        s->tail = e;
        s->entry_count++;
        s->active_tokens += e->token_in;
        ledger->entry_count++;
        ledger->total_tokens += e->token_in;
    }

    if (out_ledger_id)
        *out_ledger_id = AIRY_STRDUP(first_id);
    return AIRY_SUCCESS;
}

int mem_ledger_window(mem_ledger_t *ledger, const char *session_id, ledger_window_t *out)
{
    ledger_session_t *s;
    ledger_entry_view_t *arr;
    size_t n = 0;

    if (!ledger || !session_id || !out)
        return AIRY_ERR_INVALID_PARAM;
    AIRY_MEMSET(out, 0, sizeof(*out));

    s = session_find(ledger, session_id);
    if (!s)
        return AIRY_SUCCESS; /* 会话不存在 → 空窗口 */

    arr = AIRY_CALLOC(s->entry_count ? s->entry_count : 1, sizeof(ledger_entry_view_t));
    if (!arr)
        return AIRY_ERR_OUT_OF_MEMORY;

    for (ledger_entry_t *e = s->head; e; e = e->next) {
        if (e->status != LEDGER_STATUS_ACTIVE)
            continue;
        ledger_entry_view_t *v = view_from_entry(e);
        if (!v)
            break;
        arr[n++] = *v;
        AIRY_FREE(v);
    }

    out->entries = arr;
    out->count = n;
    out->total_tokens = s->active_tokens;
    out->warn = s->active_tokens >= (size_t)((double)s->budget * ledger->warn_ratio);
    return AIRY_SUCCESS;
}

void mem_ledger_window_free(ledger_window_t *win)
{
    if (!win)
        return;
    for (size_t i = 0; i < win->count; i++) {
        AIRY_FREE(win->entries[i].session_id);
        AIRY_FREE(win->entries[i].source);
        AIRY_FREE(win->entries[i].ref_id);
    }
    AIRY_FREE(win->entries);
    win->entries = NULL;
    win->count = 0;
}

int mem_ledger_budget(mem_ledger_t *ledger, const char *session_id,
                      size_t *out_used, size_t *out_limit, size_t *out_headroom)
{
    ledger_session_t *s;

    if (!ledger || !session_id)
        return AIRY_ERR_INVALID_PARAM;
    s = session_find(ledger, session_id);
    if (!s) {
        if (out_used) *out_used = 0;
        if (out_limit) *out_limit = ledger->default_budget;
        if (out_headroom) *out_headroom = ledger->default_budget;
        return AIRY_SUCCESS;
    }
    if (out_used) *out_used = s->active_tokens;
    if (out_limit) *out_limit = s->budget;
    if (out_headroom) *out_headroom = s->active_tokens < s->budget ? s->budget - s->active_tokens : 0;
    return AIRY_SUCCESS;
}

int mem_ledger_mark(mem_ledger_t *ledger, const char *session_id,
                    const char **entry_ids, size_t count, int status, size_t *out_updated)
{
    ledger_session_t *s;
    size_t updated = 0;

    if (!ledger || !session_id || (!entry_ids && count > 0))
        return AIRY_ERR_INVALID_PARAM;
    s = session_find(ledger, session_id);
    if (!s)
        return AIRY_ERR_NOT_FOUND;

    for (size_t i = 0; i < count; i++) {
        ledger_entry_t *e = entry_find_by_id(s, entry_ids[i]);
        if (!e)
            continue;
        /* append-only：不修改原条目，追加一条 status 变更记录 */
        ledger_entry_t *rec = AIRY_CALLOC(1, sizeof(ledger_entry_t));
        if (!rec)
            return AIRY_ERR_OUT_OF_MEMORY;
        entry_id_gen(ledger, rec->entry_id);
        rec->session_id = AIRY_STRDUP(session_id);
        rec->entry_type = e->entry_type;
        rec->token_in = 0; /* 状态变更记录不占预算 */
        rec->token_out = 0;
        rec->source = AIRY_STRDUP("ledger");
        rec->status = status;
        rec->created_at = ledger_now_ns();
        rec->ref_id = AIRY_STRDUP(e->entry_id); /* 指向被标记的原始条目 */
        rec->seq = ++ledger->seq;

        if (s->tail)
            s->tail->next = rec;
        else
            s->head = rec;
        s->tail = rec;
        s->entry_count++;
        ledger->entry_count++;

        e->status = status; /* 原条目状态迁移（active→compressed/evicted/deduped） */
        updated++;
    }

    session_recalc_active(s);
    if (out_updated)
        *out_updated = updated;
    return AIRY_SUCCESS;
}

int mem_ledger_history(mem_ledger_t *ledger, const char *session_id, size_t limit,
                       ledger_entry_view_t **out, size_t *out_count)
{
    ledger_session_t *s;
    size_t total = 0;

    if (!ledger || !session_id || !out || !out_count)
        return AIRY_ERR_INVALID_PARAM;
    *out = NULL;
    *out_count = 0;

    s = session_find(ledger, session_id);
    if (!s)
        return AIRY_SUCCESS;

    for (ledger_entry_t *e = s->head; e; e = e->next)
        total++;

    size_t take = limit > 0 && limit < total ? limit : total;
    if (take == 0)
        return AIRY_SUCCESS;

    ledger_entry_view_t *arr = AIRY_CALLOC(take, sizeof(ledger_entry_view_t));
    if (!arr)
        return AIRY_ERR_OUT_OF_MEMORY;

    /* 返回最近 take 条（新 → 旧） */
    size_t written = 0;
    ledger_entry_t *walk = s->head;
    /* 先定位到第 (total - take) 个 */
    for (size_t i = 0; i < total - take; i++)
        walk = walk->next;
    for (; walk; walk = walk->next) {
        ledger_entry_view_t *v = view_from_entry(walk);
        if (!v)
            break;
        arr[written++] = *v;
        AIRY_FREE(v);
    }

    *out = arr;
    *out_count = written;
    return AIRY_SUCCESS;
}

void mem_ledger_history_free(ledger_entry_view_t *items, size_t count)
{
    if (!items)
        return;
    for (size_t i = 0; i < count; i++) {
        AIRY_FREE(items[i].session_id);
        AIRY_FREE(items[i].source);
        AIRY_FREE(items[i].ref_id);
    }
    AIRY_FREE(items);
}

void mem_ledger_stats(mem_ledger_t *ledger, mem_ledger_stats_t *out)
{
    if (!ledger || !out)
        return;
    out->sessions = ledger->session_count;
    out->entries = ledger->entry_count;
    out->total_tokens = ledger->total_tokens;
}
