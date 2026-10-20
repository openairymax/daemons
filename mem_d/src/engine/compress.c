/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file compress.c
 * @brief 提示词压缩实现（见 compress.h）。
 *
 * L1 规则裁剪（默认开）→ L2 抽取式摘要（默认关，须过 A/B 门禁再灰度），
 * L3（LLM 摘要）按 14-prompt-compression.md §3 需 A/B 门禁达标后才灰度，
 * 本版不实现。L2 门禁判定见 mem_compress_gate_pass（fail-closed）。
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
    char *d = AIRY_STRDUP(s);
    if (d)
        v->items[v->count++] = d;
}

static void strvec_free(strvec_t *v)
{
    for (size_t i = 0; i < v->count; i++)
        AIRY_FREE(v->items[i]);
    AIRY_FREE(v->items);
    AIRY_MEMSET(v, 0, sizeof(*v));
}

/* UTF-8 字节序列前缀匹配（避免 CJK 标点多字符常量告警 + 误匹配） */
static int starts_with_utf8(const unsigned char *p, const char *seq)
{
    for (int i = 0; seq[i]; i++) {
        if (p[i] != (unsigned char)seq[i])
            return 0;
    }
    return 1;
}

/* 句子切分：按 。！？!?；; . 或换行切分 */
static void split_sentences(const char *text, strvec_t *out)
{
    const unsigned char *start = (const unsigned char *)text;
    for (const unsigned char *p = (const unsigned char *)text;; p++) {
        /* CJK 标点：。=E3 80 82，！=EF BC 81，？=EF BC 9F，；=EF BC 9B */
        int is_sep = *p == '\0' || *p == '!' || *p == '?' || *p == ';' || *p == '.' ||
                     *p == '\n' || starts_with_utf8(p, "\xE3\x80\x82") ||
                     starts_with_utf8(p, "\xEF\xBC\x81") || starts_with_utf8(p, "\xEF\xBC\x9F") ||
                     starts_with_utf8(p, "\xEF\xBC\x9B");
        if (is_sep) {
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
    if (wf->words[wf->count])
        wf->freq[wf->count++] = 1;
}

static void wordfreq_visit(void *ud, const char *word)
{
    wordfreq_add((wordfreq_t *)ud, word);
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
            strncat(out, sents.items[i], strlen(sents.items[i]));
            strncat(out, "\n", 1);
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

/* ─── L2 A/B 门禁 ─────────────────────────────────────────────────────── */

int mem_compress_gate_pass(const compress_gate_t *gate)
{
    if (!gate)
        return 0; /* fail-closed：无门禁输入不放行 */
    if (!gate->grayscale_enabled)
        return 0;
    if (!(gate->acr >= 0.0) || gate->acr > 1.0)
        return 0; /* 负数 / NaN / 越界 → 数据不可用 */
    if (!(gate->ttft_ms >= 0.0))
        return 0;
    if (gate->acr < COMPRESS_GATE_MIN_ACR)
        return 0;
    if (gate->ttft_ms > COMPRESS_GATE_MAX_TTFT_MS)
        return 0;
    return 1;
}

/* ─── 主流程 ───────────────────────────────────────────────────────────── */

/* 压缩上下文：条目窗口 + 派生缓冲 + 累计状态，供各阶段处理器共享。 */
typedef struct {
    const compress_entry_in_t *ents;
    size_t count;
    compress_config_t cfg;
    airy_token_counter_t *counter;
    int *act;
    char **repl;
    uint64_t *hashes;
    size_t *tokens;
    compress_plan_item_t *plans;
    size_t nplans;
    size_t saved;
    int *turn_of;
    int turns;
    int cur_user;
} cmp_ctx_t;

static void cmp_drop(cmp_ctx_t *x)
{
    size_t i;

    if (x->repl) {
        for (i = 0; i < x->count; i++)
            AIRY_FREE(x->repl[i]);
    }
    if (x->plans) {
        for (i = 0; i < x->nplans; i++)
            AIRY_FREE(x->plans[i].entry_id);
    }
    AIRY_FREE(x->plans);
    AIRY_FREE(x->act);
    AIRY_FREE(x->repl);
    AIRY_FREE(x->hashes);
    AIRY_FREE(x->tokens);
    AIRY_FREE(x->turn_of);
    if (x->counter)
        airy_token_counter_destroy(x->counter);
    x->act = NULL;
    x->repl = NULL;
    x->hashes = NULL;
    x->tokens = NULL;
    x->plans = NULL;
    x->turn_of = NULL;
    x->counter = NULL;
    x->nplans = 0;
}

static void cmp_release(cmp_ctx_t *x)
{
    size_t i;

    for (i = 0; i < x->count; i++)
        AIRY_FREE(x->repl[i]);
    AIRY_FREE(x->act);
    AIRY_FREE(x->repl);
    AIRY_FREE(x->hashes);
    AIRY_FREE(x->tokens);
    AIRY_FREE(x->turn_of);
    airy_token_counter_destroy(x->counter);
}

/* 默认配置：L2 与灰度门禁默认关（fail-closed，须过 A/B 门禁再灰度）。 */
static const compress_config_t cmp_defaults = {
    .max_tool_tokens = COMPRESS_DEFAULT_MAX_TOOL_TOKENS,
    .max_turns = COMPRESS_DEFAULT_MAX_TURNS,
    .l1_enabled = 1,
    .l2_enabled = 0, /* 默认关：高风险改写须过 A/B 门禁再灰度 */
    .dedup = 1,
    .gate = { .grayscale_enabled = 0, .acr = -1.0, .ttft_ms = -1.0 },
};

static void cmp_cfg(compress_config_t *dc, const compress_config_t *cfg)
{
    *dc = cmp_defaults;
    if (!cfg)
        return;
    if (cfg->max_tool_tokens > 0)
        dc->max_tool_tokens = cfg->max_tool_tokens;
    if (cfg->max_turns > 0)
        dc->max_turns = cfg->max_turns;
    dc->l1_enabled = cfg->l1_enabled;
    dc->l2_enabled = cfg->l2_enabled;
    dc->dedup = cfg->dedup;
    dc->gate = cfg->gate;
}

static int cmp_bufs(cmp_ctx_t *x, mem_ledger_t *ledger)
{
    x->act = AIRY_CALLOC(x->count, sizeof(int));
    x->repl = AIRY_CALLOC(x->count, sizeof(char *));
    x->hashes = AIRY_CALLOC(x->count, sizeof(uint64_t));
    x->tokens = AIRY_CALLOC(x->count, sizeof(size_t));
    x->plans = AIRY_CALLOC(x->count, sizeof(compress_plan_item_t));
    /* 计数模型与台账同源（声明注入；ledger 为空 → 默认模型） */
    x->counter = airy_token_counter_create(mem_ledger_tfam(ledger));
    if (x->act && x->repl && x->hashes && x->tokens && x->plans && x->counter)
        return AIRY_SUCCESS;
    cmp_drop(x);
    return AIRY_ERR_OUT_OF_MEMORY;
}

/* 条目 token：显式优先，否则 token_standard 估算；同时定位最后一条 user。 */
static void cmp_tokens(cmp_ctx_t *x, size_t *used)
{
    size_t sum = 0;
    size_t i;

    for (i = 0; i < x->count; i++) {
        if (x->ents[i].token_in > 0)
            x->tokens[i] = x->ents[i].token_in;
        else
            x->tokens[i] = x->ents[i].text
                               ? airy_token_counter_count(x->counter, x->ents[i].text)
                               : 0;
        sum += x->tokens[i];
        if (x->ents[i].entry_type == LEDGER_ENTRY_USER)
            x->cur_user = (int)i;
    }
    *used = sum;
}

/* 轮边界：user 条目即一轮起点（含后续同轮 assistant/tool_result）。 */
static int cmp_turns(cmp_ctx_t *x)
{
    int cur = 0;
    size_t i;

    x->turn_of = AIRY_CALLOC(x->count, sizeof(int));
    if (!x->turn_of) {
        cmp_drop(x);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    for (i = 0; i < x->count; i++) {
        if (x->ents[i].entry_type == LEDGER_ENTRY_USER)
            cur++;
        x->turn_of[i] = cur;
    }
    x->turns = cur;
    return AIRY_SUCCESS;
}

/* 保护规则：system / tool_def / cache_hit / 当前请求 永不压缩。 */
static int cmp_protected(const cmp_ctx_t *x, size_t i)
{
    return x->ents[i].entry_type == LEDGER_ENTRY_SYSTEM ||
           x->ents[i].entry_type == LEDGER_ENTRY_TOOL_DEF ||
           x->ents[i].entry_type == LEDGER_ENTRY_CACHE_HIT ||
           (int)i == x->cur_user;
}

/* L1-1：超轮数丢弃最早轮（保留首轮上下文 + 最近 max_turns-1 轮）。 */
static int l1_drop_turn(cmp_ctx_t *x, size_t i)
{
    int keep_tail;
    int keep_from;

    if (x->ents[i].entry_type != LEDGER_ENTRY_USER &&
        x->ents[i].entry_type != LEDGER_ENTRY_ASSISTANT &&
        x->ents[i].entry_type != LEDGER_ENTRY_TOOL_RESULT)
        return 0;
    keep_tail = (int)x->cfg.max_turns > 1 ? (int)x->cfg.max_turns - 1 : 0;
    keep_from = x->turns - keep_tail + 1; /* 最后保留轮起始号 */
    if (x->turns <= (int)x->cfg.max_turns || x->turn_of[i] <= 1 || x->turn_of[i] >= keep_from)
        return 0;
    x->act[i] = COMPRESS_ACTION_DROP;
    x->saved += x->tokens[i];
    return 1;
}

/* L1-2：tool_result 超长截断（头部 + 尾部，中间占位）。 */
static int l1_truncate(cmp_ctx_t *x, size_t i)
{
    size_t len, head, tail, nlen;
    char *buf;
    int m;

    if (x->ents[i].entry_type != LEDGER_ENTRY_TOOL_RESULT ||
        x->tokens[i] <= x->cfg.max_tool_tokens || !x->ents[i].text)
        return 0;
    len = strlen(x->ents[i].text);
    if (len <= 32)
        return 0;
    head = len * 2 / 5;
    tail = len * 2 / 5;
    if (head + tail >= len)
        return 0;
    nlen = head + tail + 48;
    buf = AIRY_MALLOC(nlen);
    if (!buf)
        return 0;
    AIRY_MEMCPY(buf, x->ents[i].text, head);
    m = snprintf(buf + head, nlen - head, "\n…[truncated %zu tokens]…\n",
                 x->tokens[i] - x->cfg.max_tool_tokens);
    if (m <= 0 || (size_t)m >= nlen - head) {
        AIRY_FREE(buf);
        return 0;
    }
    AIRY_MEMCPY(buf + head + (size_t)m, x->ents[i].text + (len - tail), tail);
    buf[head + (size_t)m + tail] = '\0';
    x->repl[i] = buf;
    x->act[i] = COMPRESS_ACTION_TRUNCATE;
    x->saved += x->tokens[i] - x->cfg.max_tool_tokens;
    return 1;
}

/* L1-3：精确去重（保留首条）。 */
static int l1_dedup(cmp_ctx_t *x, size_t i)
{
    uint64_t h;
    size_t j;

    if (!x->cfg.dedup || !x->ents[i].text)
        return 0;
    h = text_hash(x->ents[i].text);
    for (j = 0; j < i; j++) {
        if (x->act[j] != COMPRESS_ACTION_DROP && x->hashes[j] == h &&
            x->ents[j].text && strcmp(x->ents[j].text, x->ents[i].text) == 0) {
            if (x->act[i] != COMPRESS_ACTION_NONE)
                break;
            x->act[i] = COMPRESS_ACTION_DEDUP;
            x->saved += x->tokens[i];
            return 1;
        }
    }
    x->hashes[i] = h;
    return 0;
}

static void cmp_l1(cmp_ctx_t *x)
{
    size_t i;

    for (i = 0; i < x->count; i++) {
        if (cmp_protected(x, i))
            continue;
        if (x->cfg.l1_enabled) {
            if (l1_drop_turn(x, i))
                continue;
            (void)l1_truncate(x, i);
            if (l1_dedup(x, i))
                continue;
        }
        if (x->act[i] == COMPRESS_ACTION_NONE && x->ents[i].text)
            x->hashes[i] = text_hash(x->ents[i].text);
    }
}

/* L2 逐条目抽取：仅历史 assistant/user，且须实际缩短。 */
static void l2_extract(cmp_ctx_t *x, size_t i, const wordfreq_t *wf)
{
    char *summ;
    size_t new_tok;

    if (x->act[i] != COMPRESS_ACTION_NONE)
        return;
    if (x->ents[i].entry_type != LEDGER_ENTRY_ASSISTANT &&
        x->ents[i].entry_type != LEDGER_ENTRY_USER)
        return;
    if ((int)i == x->cur_user)
        return;
    if (x->turn_of[i] == 1 && x->ents[i].entry_type == LEDGER_ENTRY_USER)
        return;
    if (x->tokens[i] < 32 || !x->ents[i].text)
        return;

    summ = extract_key_sentences(x->ents[i].text, wf);
    if (summ && strcmp(summ, x->ents[i].text) != 0 &&
        strlen(summ) < strlen(x->ents[i].text)) {
        new_tok = airy_token_counter_count(x->counter, summ);
        if (new_tok < x->tokens[i]) {
            x->repl[i] = summ;
            summ = NULL;
            x->act[i] = COMPRESS_ACTION_EXTRACT;
            x->saved += x->tokens[i] - new_tok;
        }
    }
    AIRY_FREE(summ);
}

/* L2：L1 后仍超预算则对历史轮次抽取关键句。
 * 生效须双条件：显式开启 且 A/B 门禁放行（fail-closed）。 */
static void cmp_l2(cmp_ctx_t *x, mem_ledger_t *ledger, const char *session_id, size_t used)
{
    size_t budget = 0;
    size_t headroom = 0;
    size_t remaining;
    double warn_at;
    int over_budget;
    wordfreq_t wf;
    size_t i;

    if (x->cfg.l2_enabled && mem_compress_gate_pass(&x->cfg.gate)) {
        if (ledger)
            mem_ledger_budget(ledger, session_id, NULL, &budget, &headroom);
        remaining = used > x->saved ? used - x->saved : 0;
        warn_at = (double)budget * (ledger ? mem_ledger_warn_ratio(ledger) : 0.8);
        over_budget = budget > 0 && remaining >= (size_t)warn_at;
        if (over_budget || x->turns > (int)x->cfg.max_turns) {
            AIRY_MEMSET(&wf, 0, sizeof(wf));
            for (i = 0; i < x->count; i++) {
                if (x->ents[i].text)
                    airy_words_scan(x->ents[i].text, strlen(x->ents[i].text),
                                    wordfreq_visit, &wf);
            }
            for (i = 0; i < x->count; i++)
                l2_extract(x, i, &wf);
            wordfreq_free(&wf);
        }
    }
    AIRY_FREE(x->turn_of);
    x->turn_of = NULL;
}

/* 组装 actions 与重组上下文。 */
static int cmp_build(cmp_ctx_t *x, char **out_ctx)
{
    size_t ctx_len = 1;
    char *ctx;
    const char *piece;
    size_t i;

    for (i = 0; i < x->count; i++) {
        if (x->act[i] == COMPRESS_ACTION_DROP || x->act[i] == COMPRESS_ACTION_DEDUP)
            continue;
        piece = x->repl[i] ? x->repl[i] : (x->ents[i].text ? x->ents[i].text : "");
        ctx_len += strlen(piece) + 1;
    }
    ctx = AIRY_MALLOC(ctx_len);
    if (!ctx) {
        cmp_drop(x);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    ctx[0] = '\0';
    for (i = 0; i < x->count; i++) {
        /* 动作记录先行：DROP/DEDUP 也须返回（调用方据此 mark/审计） */
        if (x->act[i] != COMPRESS_ACTION_NONE) {
            x->plans[x->nplans].entry_id = AIRY_STRDUP(x->ents[i].entry_id);
            x->plans[x->nplans].entry_type = x->ents[i].entry_type;
            x->plans[x->nplans].action = x->act[i];
            x->nplans++;
        }
        if (x->act[i] == COMPRESS_ACTION_DROP || x->act[i] == COMPRESS_ACTION_DEDUP)
            continue; /* 不进重组上下文 */
        piece = x->repl[i] ? x->repl[i] : (x->ents[i].text ? x->ents[i].text : "");
        if (ctx[0])
            strncat(ctx, "\n", 1);
        strncat(ctx, piece, strlen(piece));
    }
    *out_ctx = ctx;
    return AIRY_SUCCESS;
}

static size_t cmp_saved(const cmp_ctx_t *x, const char *ctx, size_t used)
{
    size_t new_tok;

    if (x->saved == 0)
        return 0;
    new_tok = airy_token_counter_count(x->counter, ctx);
    return used > new_tok ? used - new_tok : x->saved;
}

static void cmp_emit(cmp_ctx_t *x, char *ctx, size_t saved, char **out_context,
                     size_t *out_saved, compress_plan_item_t **out_plans, size_t *out_count)
{
    size_t i;

    if (out_context)
        *out_context = ctx;
    else
        AIRY_FREE(ctx);
    if (out_saved)
        *out_saved = saved;
    if (out_plans) {
        *out_plans = x->plans;
    } else {
        for (i = 0; i < x->nplans; i++)
            AIRY_FREE(x->plans[i].entry_id);
        AIRY_FREE(x->plans);
    }
    if (out_count)
        *out_count = x->nplans;
}

int mem_compress_plan(mem_ledger_t *ledger, const char *session_id,
                      const compress_entry_in_t *entries, size_t count,
                      const compress_config_t *cfg,
                      char **out_context, size_t *out_saved_tokens,
                      compress_plan_item_t **out_actions, size_t *out_action_count)
{
    cmp_ctx_t x;
    size_t used = 0;
    char *ctx = NULL;
    size_t saved;
    int err;

    if (!entries && count > 0)
        return AIRY_ERR_INVALID_PARAM;
    if (out_context) *out_context = NULL;
    if (out_saved_tokens) *out_saved_tokens = 0;
    if (out_actions) *out_actions = NULL;
    if (out_action_count) *out_action_count = 0;

    AIRY_MEMSET(&x, 0, sizeof(x));
    x.ents = entries;
    x.count = count;
    x.cur_user = -1;
    cmp_cfg(&x.cfg, cfg);
    if (count == 0)
        return AIRY_SUCCESS; /* 空窗口 → 无压缩 */

    err = cmp_bufs(&x, ledger);
    if (err == AIRY_SUCCESS)
        err = cmp_turns(&x);
    if (err != AIRY_SUCCESS)
        return err;

    cmp_tokens(&x, &used);

    cmp_l1(&x);
    cmp_l2(&x, ledger, session_id, used);

    err = cmp_build(&x, &ctx);
    if (err != AIRY_SUCCESS)
        return err;

    saved = cmp_saved(&x, ctx, used);
    cmp_release(&x);
    cmp_emit(&x, ctx, saved, out_context, out_saved_tokens, out_actions, out_action_count);
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
