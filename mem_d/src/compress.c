/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file compress.c
 * @brief 提示词压缩实现（见 compress.h）。
 *
 * L1 规则裁剪（默认开）→ L2 抽取式摘要（默认开），L3（LLM 摘要）按
 * 14-prompt-compression.md §3 需 A/B 门禁达标后才灰度，本版不实现。
 * 压缩产物与原始条目映射保留于台账（ref_id），可经 ledger.history 回放。
 */

#include "compress.h"

#include "airy_memory.h"
#include "token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── 内部辅助 ─────────────────────────────────────────────────────────── */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} strvec_t;

static void strvec_push(strvec_t *v, const char *s)
{
    if (v->count == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 8;
        char **ni = AIRY_REALLOC(v->items, sizeof(char *) * ncap);
        if (!ni)
            return;
        v->items = ni;
        v->cap = ncap;
    }
    v->items[v->count++] = AIRY_STRDUP(s);
}

static void strvec_free(strvec_t *v)
{
    for (size_t i = 0; i < v->count; i++)
        AIRY_FREE(v->items[i]);
    AIRY_FREE(v->items);
    AIRY_MEMSET(v, 0, sizeof(*v));
}

/* 句子切分：按 。！？!?；; . 或换行切分 */
static void split_sentences(const char *text, strvec_t *out)
{
    const char *start = text;
    for (const char *p = text;; p++) {
        if (*p == '\0' || *p == '。' || *p == '！' || *p == '？' || *p == '!' ||
            *p == '?' || *p == '；' || *p == ';' || *p == '.' || *p == '\n') {
            if (p > start) {
                size_t len = (size_t)(p - start);
                char *buf = AIRY_MALLOC(len + 1);
                if (buf) {
                    AIRY_MEMCPY(buf, start, len);
                    buf[len] = '\0';
                    int blank = 1;
                    for (char *q = buf; *q; q++) {
                        if (*q != ' ' && *q != '\t' && *q != '\r') {
                            blank = 0;
                            break;
                        }
                    }
                    if (!blank)
                        strvec_push(out, buf);
                    AIRY_FREE(buf);
                }
            }
            if (*p == '\0')
                break;
            start = p + 1;
        }
    }
}

/* 指令动词集合 */
static int is_instruction_word(const char *w)
{
    static const char *const words[] = {"please", "ensure",  "must",  "always", "never",
                                        "verify", "confirm", "注意", "确保",   "需要",
                                        "必须",   "请",      "务必", "不要"};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strcmp(w, words[i]) == 0)
            return 1;
    }
    return 0;
}

typedef struct {
    char **words;
    int *freq;
    size_t count;
    size_t cap;
} wordfreq_t;

static void wordfreq_add(wordfreq_t *wf, const char *word)
{
    for (size_t i = 0; i < wf->count; i++) {
        if (strcmp(wf->words[i], word) == 0) {
            wf->freq[i]++;
            return;
        }
    }
    if (wf->count == wf->cap) {
        size_t ncap = wf->cap ? wf->cap * 2 : 32;
        char **nw = AIRY_REALLOC(wf->words, sizeof(char *) * ncap);
        int *nf = AIRY_REALLOC(wf->freq, sizeof(int) * ncap);
        if (!nw || !nf) {
            AIRY_FREE(nw);
            AIRY_FREE(nf);
            return;
        }
        wf->words = nw;
        wf->freq = nf;
        wf->cap = ncap;
    }
    wf->words[wf->count] = AIRY_STRDUP(word);
    wf->freq[wf->count] = 1;
    wf->count++;
}

static void wordfreq_build(const char *text, wordfreq_t *wf)
{
    const unsigned char *p = (const unsigned char *)text;
    char word[128];
    size_t wlen = 0;
    while (*p) {
        unsigned char c = *p;
        if (c >= 0x80) {
            size_t seq = 1;
            if ((c & 0xE0) == 0xC0) seq = 2;
            else if ((c & 0xF0) == 0xE0) seq = 3;
            else if ((c & 0xF8) == 0xF0) seq = 4;
            char buf[5] = {0};
            size_t avail = 0;
            for (size_t i = 0; i < seq; i++) {
                if (!p[i]) break;
                buf[i] = (char)p[i];
                avail++;
            }
            buf[avail] = '\0';
            if (avail >= 2) {
                wordfreq_add(wf, buf);
                p += avail;
                continue;
            }
            p++;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (wlen < sizeof(word) - 1)
                word[wlen++] = (char)c;
        } else if (c >= 'A' && c <= 'Z') {
            if (wlen < sizeof(word) - 1)
                word[wlen++] = (char)(c + 32);
        } else {
            if (wlen > 0) {
                word[wlen] = '\0';
                wordfreq_add(wf, word);
                wlen = 0;
            }
        }
        p++;
    }
    if (wlen > 0) {
        word[wlen] = '\0';
        wordfreq_add(wf, word);
    }
}

static int wordfreq_get(const wordfreq_t *wf, const char *word)
{
    for (size_t i = 0; i < wf->count; i++) {
        if (strcmp(wf->words[i], word) == 0)
            return wf->freq[i];
    }
    return 0;
}

static void wordfreq_free(wordfreq_t *wf)
{
    for (size_t i = 0; i < wf->count; i++)
        AIRY_FREE(wf->words[i]);
    AIRY_FREE(wf->words);
    AIRY_FREE(wf->freq);
    AIRY_MEMSET(wf, 0, sizeof(*wf));
}

/* 抽取关键句：位置加权 + 高频术语 + 指令动词，取 top K 句 */
static char *extract_key_sentences(const char *text, const wordfreq_t *wf)
{
    strvec_t sents;
    AIRY_MEMSET(&sents, 0, sizeof(sents));
    split_sentences(text, &sents);
    if (sents.count <= 2) {
        strvec_free(&sents);
        return AIRY_STRDUP(text);
    }

    size_t n = sents.count;
    double *scores = AIRY_CALLOC(n, sizeof(double));
    int *picked = AIRY_CALLOC(n, sizeof(int));
    if (!scores || !picked) {
        AIRY_FREE(scores);
        AIRY_FREE(picked);
        strvec_free(&sents);
        return AIRY_STRDUP(text);
    }

    for (size_t i = 0; i < n; i++) {
        double s = 0.0;
        if (i == 0)
            s += 0.3; /* 首句：主题陈述 */
        if (i == n - 1)
            s += 0.2; /* 末句：结论/追问 */
        const unsigned char *p = (const unsigned char *)sents.items[i];
        char word[128];
        size_t wlen = 0;
        while (*p) {
            unsigned char c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                if (wlen < sizeof(word) - 1)
                    word[wlen++] = (char)c;
            } else if (c >= 'A' && c <= 'Z') {
                if (wlen < sizeof(word) - 1)
                    word[wlen++] = (char)(c + 32);
            } else {
                if (wlen > 0) {
                    word[wlen] = '\0';
                    if (wordfreq_get(wf, word) > 1)
                        s += 0.1;
                    if (is_instruction_word(word))
                        s += 0.5;
                    wlen = 0;
                }
            }
            p++;
        }
        if (wlen > 0) {
            word[wlen] = '\0';
            if (wordfreq_get(wf, word) > 1)
                s += 0.1;
            if (is_instruction_word(word))
                s += 0.5;
        }
        scores[i] = s;
    }

    size_t k = n / 2;
    if (k < 2) k = 2;
    if (k > 3) k = 3;
    for (size_t t = 0; t < k; t++) {
        size_t best = n;
        for (size_t i = 0; i < n; i++) {
            if (picked[i])
                continue;
            if (best == n || scores[i] > scores[best])
                best = i;
        }
        if (best != n)
            picked[best] = 1;
    }

    size_t total = 0;
    for (size_t i = 0; i < n; i++)
        if (picked[i])
            total += strlen(sents.items[i]) + 1;
    char *out = AIRY_MALLOC(total + 1);
    if (out) {
        out[0] = '\0';
        for (size_t i = 0; i < n; i++) {
            if (!picked[i])
                continue;
            strncat(out, sents.items[i], total);
            strncat(out, "\n", 2);
        }
    }

    AIRY_FREE(scores);
    AIRY_FREE(picked);
    strvec_free(&sents);
    return out ? out : AIRY_STRDUP(text);
}

/* 简单 FNV-1a 文本哈希（去重用） */
static uint64_t text_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

/* ─── 主流程 ───────────────────────────────────────────────────────────── */

int mem_compress_plan(mem_ledger_t *ledger, const char *session_id,
                      const compress_entry_in_t *entries, size_t count,
                      const compress_config_t *cfg,
                      char **out_context, size_t *out_saved_tokens,
                      compress_plan_item_t **out_actions, size_t *out_action_count)
{
    compress_config_t dc = {
        .max_tool_tokens = COMPRESS_DEFAULT_MAX_TOOL_TOKENS,
        .max_turns = COMPRESS_DEFAULT_MAX_TURNS,
        .l1_enabled = 1,
        .l2_enabled = 1,
        .dedup = 1,
    };
    size_t i;
    int *act;
    char **repl;
    uint64_t *hashes;
    compress_plan_item_t *actions;
    airy_token_counter_t *counter = NULL;
    size_t action_count = 0;
    size_t *tokens;

    if (!entries && count > 0)
        return AIRY_ERR_INVALID_PARAM;
    if (out_context) *out_context = NULL;
    if (out_saved_tokens) *out_saved_tokens = 0;
    if (out_actions) *out_actions = NULL;
    if (out_action_count) *out_action_count = 0;
    if (cfg) {
        if (cfg->max_tool_tokens > 0) dc.max_tool_tokens = cfg->max_tool_tokens;
        if (cfg->max_turns > 0) dc.max_turns = cfg->max_turns;
        dc.l1_enabled = cfg->l1_enabled;
        dc.l2_enabled = cfg->l2_enabled;
        dc.dedup = cfg->dedup;
    }
    if (count == 0)
        return AIRY_SUCCESS; /* 空窗口 → 无压缩 */

    act = AIRY_CALLOC(count, sizeof(int));
    repl = AIRY_CALLOC(count, sizeof(char *));
    hashes = AIRY_CALLOC(count, sizeof(uint64_t));
    tokens = AIRY_CALLOC(count, sizeof(size_t));
    actions = AIRY_CALLOC(count, sizeof(compress_plan_item_t));
    counter = airy_token_counter_create("gpt-4");
    if (!act || !repl || !hashes || !tokens || !actions || !counter) {
        AIRY_FREE(act);
        for (i = 0; i < count; i++) AIRY_FREE(repl ? repl[i] : NULL);
        AIRY_FREE(repl);
        AIRY_FREE(hashes);
        AIRY_FREE(tokens);
        AIRY_FREE(actions);
        if (counter) airy_token_counter_destroy(counter);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    /* 条目 token：显式优先，否则 token_standard 估算 */
    size_t used = 0;
    for (i = 0; i < count; i++) {
        if (entries[i].token_in > 0)
            tokens[i] = entries[i].token_in;
        else
            tokens[i] = entries[i].text
                            ? airy_token_counter_count(counter, entries[i].text)
                            : 0;
        used += tokens[i];
    }

    /* 识别当前请求：最后一条 user */
    int current_user_idx = -1;
    for (i = 0; i < count; i++) {
        if (entries[i].entry_type == LEDGER_ENTRY_USER)
            current_user_idx = (int)i;
    }

    /* 轮边界：user 条目即一轮起点（含后续同轮 assistant/tool_result） */
    int *turn_of = AIRY_CALLOC(count, sizeof(int));
    if (!turn_of) {
        AIRY_FREE(act);
        for (i = 0; i < count; i++) AIRY_FREE(repl[i]);
        AIRY_FREE(repl);
        AIRY_FREE(hashes);
        AIRY_FREE(tokens);
        AIRY_FREE(actions);
        airy_token_counter_destroy(counter);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    int cur_turn = 0;
    for (i = 0; i < count; i++) {
        if (entries[i].entry_type == LEDGER_ENTRY_USER)
            cur_turn++;
        turn_of[i] = cur_turn;
    }
    int total_turns = cur_turn;

    /* L1：逐条目判定（保护规则：system/tool_def/当前请求 永不压缩） */
    size_t l1_saved = 0;
    for (i = 0; i < count; i++) {
        int protected_entry =
            entries[i].entry_type == LEDGER_ENTRY_SYSTEM ||
            entries[i].entry_type == LEDGER_ENTRY_TOOL_DEF ||
            entries[i].entry_type == LEDGER_ENTRY_CACHE_HIT || (int)i == current_user_idx;
        if (protected_entry)
            continue;

        if (dc.l1_enabled) {
            /* 1) 超轮数丢弃最早轮（保留首轮上下文 + 最近 max_turns-1 轮） */
            if (entries[i].entry_type == LEDGER_ENTRY_USER ||
                entries[i].entry_type == LEDGER_ENTRY_ASSISTANT ||
                entries[i].entry_type == LEDGER_ENTRY_TOOL_RESULT) {
                int keep_tail = (int)dc.max_turns > 1 ? (int)dc.max_turns - 1 : 0;
                int keep_from = total_turns - keep_tail + 1; /* 最后保留轮起始号 */
                if (total_turns > (int)dc.max_turns && turn_of[i] > 1 && turn_of[i] < keep_from) {
                    act[i] = COMPRESS_ACTION_DROP;
                    l1_saved += tokens[i];
                    continue;
                }
            }

            /* 2) tool_result 超长截断（头部 + 尾部，中间占位） */
            if (entries[i].entry_type == LEDGER_ENTRY_TOOL_RESULT && tokens[i] > dc.max_tool_tokens &&
                entries[i].text) {
                size_t len = strlen(entries[i].text);
                if (len > 32) {
                    size_t head = len * 2 / 5;
                    size_t tail = len * 2 / 5;
                    if (head + tail < len) {
                        size_t nlen = head + tail + 48;
                        char *buf = AIRY_MALLOC(nlen);
                        if (buf) {
                            AIRY_MEMCPY(buf, entries[i].text, head);
                            int m = snprintf(buf + head, nlen - head,
                                             "\n…[truncated %zu tokens]…\n",
                                             tokens[i] - dc.max_tool_tokens);
                            if (m > 0 && (size_t)m < nlen - head) {
                                AIRY_MEMCPY(buf + head + (size_t)m, entries[i].text + (len - tail),
                                            tail);
                                buf[head + (size_t)m + tail] = '\0';
                                repl[i] = buf;
                                act[i] = COMPRESS_ACTION_TRUNCATE;
                                l1_saved += tokens[i] - dc.max_tool_tokens;
                            } else {
                                AIRY_FREE(buf);
                            }
                        }
                    }
                }
            }

            /* 3) 精确去重（保留首条） */
            if (dc.dedup && entries[i].text) {
                uint64_t h = text_hash(entries[i].text);
                int dup = 0;
                for (size_t j = 0; j < i; j++) {
                    if (act[j] != COMPRESS_ACTION_DROP && hashes[j] == h &&
                        entries[j].text && strcmp(entries[j].text, entries[i].text) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (dup && act[i] == COMPRESS_ACTION_NONE) {
                    act[i] = COMPRESS_ACTION_DEDUP;
                    l1_saved += tokens[i];
                    continue;
                }
                hashes[i] = h;
            }
        }

        if (act[i] == COMPRESS_ACTION_NONE && entries[i].text)
            hashes[i] = text_hash(entries[i].text);
    }

    /* L2：L1 后仍超预算则对历史 assistant/user 抽取关键句 */
    if (dc.l2_enabled) {
        size_t budget = 0, headroom = 0;
        if (ledger)
            mem_ledger_budget(ledger, session_id, NULL, &budget, &headroom);
        size_t remaining = used > l1_saved ? used - l1_saved : 0;
        double warn_at = (double)budget * 0.8;
        int over_budget = budget > 0 && remaining >= (size_t)warn_at;
        if (over_budget || total_turns > (int)dc.max_turns) {
            wordfreq_t wf;
            AIRY_MEMSET(&wf, 0, sizeof(wf));
            for (i = 0; i < count; i++) {
                if (entries[i].text)
                    wordfreq_build(entries[i].text, &wf);
            }
            for (i = 0; i < count; i++) {
                if (act[i] != COMPRESS_ACTION_NONE)
                    continue;
                if (entries[i].entry_type != LEDGER_ENTRY_ASSISTANT &&
                    entries[i].entry_type != LEDGER_ENTRY_USER)
                    continue;
                if ((int)i == current_user_idx)
                    continue;
                if (turn_of[i] == 1 && entries[i].entry_type == LEDGER_ENTRY_USER)
                    continue;
                if (tokens[i] < 32 || !entries[i].text)
                    continue;
                char *summ = extract_key_sentences(entries[i].text, &wf);
                if (summ && strcmp(summ, entries[i].text) != 0 &&
                    strlen(summ) < strlen(entries[i].text)) {
                    size_t new_tok = airy_token_counter_count(counter, summ);
                    if (new_tok < tokens[i]) {
                        repl[i] = summ;
                        summ = NULL;
                        act[i] = COMPRESS_ACTION_EXTRACT;
                        l1_saved += tokens[i] - new_tok;
                    }
                }
                AIRY_FREE(summ);
            }
            wordfreq_free(&wf);
        }
    }
    AIRY_FREE(turn_of);

    /* 组装 actions 与重组上下文 */
    size_t ctx_len = 1;
    for (i = 0; i < count; i++) {
        if (act[i] == COMPRESS_ACTION_DROP || act[i] == COMPRESS_ACTION_DEDUP)
            continue;
        const char *piece = repl[i] ? repl[i] : (entries[i].text ? entries[i].text : "");
        ctx_len += strlen(piece) + 1;
    }
    char *ctx = AIRY_MALLOC(ctx_len);
    if (!ctx) {
        for (i = 0; i < count; i++) AIRY_FREE(repl[i]);
        AIRY_FREE(act);
        AIRY_FREE(repl);
        AIRY_FREE(hashes);
        AIRY_FREE(tokens);
        AIRY_FREE(actions);
        airy_token_counter_destroy(counter);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    ctx[0] = '\0';
    for (i = 0; i < count; i++) {
        /* 动作记录先行：DROP/DEDUP 也须返回（调用方据此 mark/审计） */
        if (act[i] != COMPRESS_ACTION_NONE) {
            actions[action_count].entry_id = AIRY_STRDUP(entries[i].entry_id);
            actions[action_count].entry_type = entries[i].entry_type;
            actions[action_count].action = act[i];
            action_count++;
        }
        if (act[i] == COMPRESS_ACTION_DROP || act[i] == COMPRESS_ACTION_DEDUP)
            continue; /* 不进重组上下文 */
        const char *piece = repl[i] ? repl[i] : (entries[i].text ? entries[i].text : "");
        if (ctx[0])
            strncat(ctx, "\n", 2);
        strncat(ctx, piece, ctx_len);
    }

    size_t saved = 0;
    if (l1_saved > 0) {
        size_t new_tok = airy_token_counter_count(counter, ctx);
        saved = used > new_tok ? used - new_tok : l1_saved;
    }

    for (i = 0; i < count; i++)
        AIRY_FREE(repl[i]);
    AIRY_FREE(act);
    AIRY_FREE(repl);
    AIRY_FREE(hashes);
    AIRY_FREE(tokens);
    airy_token_counter_destroy(counter);

    if (out_context) *out_context = ctx;
    else AIRY_FREE(ctx);
    if (out_saved_tokens) *out_saved_tokens = saved;
    if (out_actions) *out_actions = actions;
    else {
        for (i = 0; i < action_count; i++) AIRY_FREE(actions[i].entry_id);
        AIRY_FREE(actions);
    }
    if (out_action_count) *out_action_count = action_count;
    return AIRY_SUCCESS;
}

void mem_compress_plan_free(char *context, compress_plan_item_t *actions, size_t action_count)
{
    AIRY_FREE(context);
    if (actions) {
        for (size_t i = 0; i < action_count; i++)
            AIRY_FREE(actions[i].entry_id);
        AIRY_FREE(actions);
    }
}
