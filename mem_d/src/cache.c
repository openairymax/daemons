/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file cache.c
 * @brief 语义缓存实现（见 cache.h）。
 *
 * 存储：自包含哈希表（exact_key 链地址法），无外部存储依赖，
 * 与 mem_d records 引擎解耦（缓存为易失层，重启可重建）。
 * 淘汰：TTL 过期优先，其次按 access_count 升序 / last_access 最旧（LRU）。
 */

#include "cache.h"
#include "airy_memory.h"
#include "error.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CACHE_HASH_BUCKETS 512
#define CACHE_ID_HEX 32
#define SHA256_HEX 64
#define DEFAULT_MAX_ENTRIES 4096UL
#define DEFAULT_MAX_BYTES (64UL * 1024UL * 1024UL)
#define DEFAULT_TTL_MS 3600000ULL
#define DEFAULT_THRESHOLD 0.85

/* ─── SHA-256（自包含，标准实现） ─────────────────────────────────────── */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    size_t datalen;
} sha256_ctx_t;

static const uint32_t SHA256_K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL,
    0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL,
    0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL,
    0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL, 0x983e5152UL,
    0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
    0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL,
    0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL,
    0xd6990624UL, 0xf40e3585UL, 0x106aa070UL, 0x19a4c116UL, 0x1e376c08UL,
    0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL,
    0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | (uint32_t)data[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;
        t1 = h + S1 + ch + SHA256_K[i] + w[i];
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667UL;
    ctx->state[1] = 0xbb67ae85UL;
    ctx->state[2] = 0x3c6ef372UL;
    ctx->state[3] = 0xa54ff53aUL;
    ctx->state[4] = 0x510e527fUL;
    ctx->state[5] = 0x9b05688cUL;
    ctx->state[6] = 0x1f83d9abUL;
    ctx->state[7] = 0x5be0cd19UL;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32])
{
    size_t i;
    uint64_t bitlen = ctx->bitlen + (uint64_t)ctx->datalen * 8;

    ctx->data[ctx->datalen++] = 0x80;
    if (ctx->datalen > 56) {
        while (ctx->datalen < 64)
            ctx->data[ctx->datalen++] = 0;
        sha256_transform(ctx, ctx->data);
        ctx->datalen = 0;
    }
    while (ctx->datalen < 56)
        ctx->data[ctx->datalen++] = 0;
    for (i = 0; i < 8; i++)
        ctx->data[63 - i] = (uint8_t)(bitlen >> (i * 8));
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 32; i++)
        hash[i] = (uint8_t)(ctx->state[i >> 2] >> (24 - (i & 3) * 8));
}

static void sha256_hex(const char *input, char out_hex[SHA256_HEX + 1])
{
    sha256_ctx_t ctx;
    uint8_t digest[32];
    static const char hex[] = "0123456789abcdef";
    int i;

    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)input, strlen(input));
    sha256_final(&ctx, digest);

    for (i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[digest[i] >> 4];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out_hex[64] = '\0';
}

/* ─── 缓存条目与表 ─────────────────────────────────────────────────────── */

typedef struct cache_entry {
    char cache_id[CACHE_ID_HEX + 1];
    char exact_key[SHA256_HEX + 1];
    char *text;
    char *response;
    char *model_id;
    uint64_t created_at;   /* monotonic ns */
    uint64_t ttl_ms;       /* 0 = 不过期 */
    uint64_t last_access;  /* monotonic ns */
    size_t access_count;
    size_t bytes;
    struct cache_entry *next; /* 链地址法 */
} cache_entry_t;

struct mem_cache {
    cache_entry_t *buckets[CACHE_HASH_BUCKETS];
    size_t count;
    size_t max_entries;
    size_t max_bytes;
    size_t bytes;
    uint64_t default_ttl_ms;
    double semantic_threshold;
    size_t hits;
    size_t misses;
    size_t evictions;
    uint64_t monotonic_seed;
    unsigned long seq;
};

static uint64_t cache_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t cache_hash64(const char *s)
{
    uint64_t h = 1469598103934665603ULL; /* FNV-1a */
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static cache_entry_t *entry_find_exact(mem_cache_t *cache, const char *exact_key)
{
    uint64_t h = cache_hash64(exact_key) % CACHE_HASH_BUCKETS;
    for (cache_entry_t *e = cache->buckets[h]; e; e = e->next) {
        if (strcmp(e->exact_key, exact_key) == 0)
            return e;
    }
    return NULL;
}

/* ─── 语义相似度（L1） ─────────────────────────────────────────────────── */

/* 简单 token 化：ASCII 按 [^a-z0-9] 分割（小写），CJK 单字一个 token */
typedef struct {
    char **tokens;
    size_t count;
} token_set_t;

static void token_set_build(const char *text, token_set_t *out)
{
    size_t cap = 64;
    out->tokens = AIRY_MALLOC(sizeof(char *) * cap);
    out->count = 0;
    if (!out->tokens)
        return;

    const unsigned char *p = (const unsigned char *)text;
    char word[128];
    size_t wlen = 0;

    while (*p) {
        unsigned char c = *p;
        if (c >= 0x80) {
            /* UTF-8 多字节序列（CJK 等）：整字作为一个 token */
            size_t seq = 1;
            if ((c & 0xE0) == 0xC0) seq = 2;
            else if ((c & 0xF0) == 0xE0) seq = 3;
            else if ((c & 0xF8) == 0xF0) seq = 4;
            if (wlen > 0) {
                word[wlen] = '\0';
                if (out->count == cap) {
                    cap *= 2;
                    char **nt = AIRY_REALLOC(out->tokens, sizeof(char *) * cap);
                    if (!nt) break;
                    out->tokens = nt;
                }
                out->tokens[out->count++] = AIRY_STRDUP(word);
                wlen = 0;
            }
            /* 单字节 UTF-8 前缀 */
            if (seq <= 4 && p[1] && seq >= 2) {
                char buf[5] = {0};
                size_t avail = 0;
                for (size_t i = 0; i < seq; i++) {
                    if (!p[i]) break;
                    buf[i] = (char)p[i];
                    avail++;
                }
                buf[avail] = '\0';
                if (out->count == cap) {
                    cap *= 2;
                    char **nt = AIRY_REALLOC(out->tokens, sizeof(char *) * cap);
                    if (!nt) break;
                    out->tokens = nt;
                }
                out->tokens[out->count++] = AIRY_STRDUP(buf);
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
                if (out->count == cap) {
                    cap *= 2;
                    char **nt = AIRY_REALLOC(out->tokens, sizeof(char *) * cap);
                    if (!nt) break;
                    out->tokens = nt;
                }
                out->tokens[out->count++] = AIRY_STRDUP(word);
                wlen = 0;
            }
        }
        p++;
    }
    if (wlen > 0) {
        word[wlen] = '\0';
        if (out->count == cap) {
            cap *= 2;
            char **nt = AIRY_REALLOC(out->tokens, sizeof(char *) * cap);
            if (nt)
                out->tokens = nt;
        }
        out->tokens[out->count++] = AIRY_STRDUP(word);
    }
}

static void token_set_free(token_set_t *set)
{
    for (size_t i = 0; i < set->count; i++)
        AIRY_FREE(set->tokens[i]);
    AIRY_FREE(set->tokens);
    set->tokens = NULL;
    set->count = 0;
}

static int token_contains(const token_set_t *set, const char *tok)
{
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->tokens[i], tok) == 0)
            return 1;
    }
    return 0;
}

/* 得分 = 0.8*jaccard + 0.2*长度比；两者任一过低直接压低得分防误命中 */
static double semantic_score(const char *a, const char *b)
{
    token_set_t sa, sb;
    size_t inter = 0, uni = 0;
    size_t la = strlen(a), lb = strlen(b);
    double jaccard, len_ratio;

    if (la == 0 || lb == 0)
        return 0.0;

    token_set_build(a, &sa);
    token_set_build(b, &sb);

    if (sa.count == 0 && sb.count == 0) {
        token_set_free(&sa);
        token_set_free(&sb);
        return 0.0;
    }
    if (sa.count == 0 || sb.count == 0) {
        token_set_free(&sa);
        token_set_free(&sb);
        return 0.0;
    }

    for (size_t i = 0; i < sa.count; i++) {
        if (token_contains(&sb, sa.tokens[i]))
            inter++;
    }
    uni = sa.count + sb.count - inter;
    jaccard = uni > 0 ? (double)inter / (double)uni : 0.0;
    len_ratio = la < lb ? (double)la / (double)lb : (double)lb / (double)la;

    token_set_free(&sa);
    token_set_free(&sb);

    return 0.8 * jaccard + 0.2 * len_ratio;
}

/* ─── 生命周期 ─────────────────────────────────────────────────────────── */

mem_cache_t *mem_cache_create(size_t max_entries, size_t max_bytes,
                              uint64_t default_ttl_ms, double semantic_threshold)
{
    mem_cache_t *cache = AIRY_CALLOC(1, sizeof(mem_cache_t));
    if (!cache)
        return NULL;
    cache->max_entries = max_entries > 0 ? max_entries : DEFAULT_MAX_ENTRIES;
    cache->max_bytes = max_bytes > 0 ? max_bytes : DEFAULT_MAX_BYTES;
    cache->default_ttl_ms = default_ttl_ms > 0 ? default_ttl_ms : DEFAULT_TTL_MS;
    cache->semantic_threshold = semantic_threshold > 0.0 && semantic_threshold <= 1.0
                                    ? semantic_threshold
                                    : DEFAULT_THRESHOLD;
    cache->seq = 0;
    return cache;
}

void mem_cache_destroy(mem_cache_t *cache)
{
    if (!cache)
        return;
    for (int i = 0; i < CACHE_HASH_BUCKETS; i++) {
        cache_entry_t *e = cache->buckets[i];
        while (e) {
            cache_entry_t *next = e->next;
            AIRY_FREE(e->text);
            AIRY_FREE(e->response);
            AIRY_FREE(e->model_id);
            AIRY_FREE(e);
            e = next;
        }
    }
    AIRY_FREE(cache);
}

static void entry_unlink(mem_cache_t *cache, cache_entry_t *prev, cache_entry_t *e)
{
    uint64_t h = cache_hash64(e->exact_key) % CACHE_HASH_BUCKETS;
    if (prev)
        prev->next = e->next;
    else
        cache->buckets[h] = e->next;
    cache->bytes -= e->bytes;
    cache->count--;
    cache->evictions++;
    AIRY_FREE(e->text);
    AIRY_FREE(e->response);
    AIRY_FREE(e->model_id);
    AIRY_FREE(e);
}

/* 淘汰：先过期，再按 access_count 升序 + last_access 最旧（LRU） */
static void cache_evict(mem_cache_t *cache)
{
    uint64_t now = cache_now_ns();

    /* 1) TTL 过期 */
    for (int i = 0; i < CACHE_HASH_BUCKETS; i++) {
        cache_entry_t *prev = NULL;
        cache_entry_t *e = cache->buckets[i];
        while (e) {
            if (e->ttl_ms > 0 && now - e->created_at >= e->ttl_ms * 1000000ULL) {
                cache_entry_t *victim = e;
                e = e->next;
                entry_unlink(cache, prev, victim);
                continue;
            }
            prev = e;
            e = e->next;
        }
    }

    /* 2) 容量超限 → LRU（access_count 最小 + last_access 最旧）。
     *    用「>」而非「>=」：put 后 count 至多 max_entries+1，淘汰恰好
     *    降到上限即可；若用 >=，count==上限时（不满但等于）也会触发并
     *    继续超量淘汰（count=上限+1 时一次淘汰多个）。 */
    while (cache->count > cache->max_entries ||
           (cache->max_bytes > 0 && cache->bytes > cache->max_bytes)) {
        cache_entry_t *victim = NULL, *vprev = NULL;
        for (int i = 0; i < CACHE_HASH_BUCKETS; i++) {
            cache_entry_t *prev = NULL; /* 每 bucket 重置：victim 前驱只在同链内有效 */
            cache_entry_t *e = cache->buckets[i];
            while (e) {
                if (!victim || e->access_count < victim->access_count ||
                    (e->access_count == victim->access_count && e->last_access < victim->last_access)) {
                    victim = e;
                    vprev = prev;
                }
                prev = e;
                e = e->next;
            }
        }
        if (!victim)
            break;
        entry_unlink(cache, vprev, victim);
    }
}

static void cache_id_gen(mem_cache_t *cache, char out[CACHE_ID_HEX + 1])
{
    uint64_t now = cache_now_ns();
    uint64_t r = (uint64_t)rand() ^ (uint64_t)(uintptr_t)cache;
    snprintf(out, CACHE_ID_HEX + 1, "%08x%08x%08x%08x",
             (uint32_t)(now & 0xFFFFFFFFUL), (uint32_t)((now >> 32) & 0xFFFFFFFFUL),
             (uint32_t)(r & 0xFFFFFFFFUL), (uint32_t)((unsigned long)++cache->seq & 0xFFFFFFFFUL));
}

int mem_cache_put(mem_cache_t *cache, const char *text, const char *response,
                  const char *model_id, uint64_t ttl_ms,
                  char **out_cache_id, char **out_exact_key)
{
    char exact_key[SHA256_HEX + 1];
    cache_entry_t *e;
    uint64_t h;

    if (!cache || !text || !response || !model_id)
        return AIRY_ERR_INVALID_PARAM;

    {
        size_t tl = strlen(text), ml = strlen(model_id);
        char *key_src = AIRY_MALLOC(tl + ml + 2);
        if (!key_src)
            return AIRY_ERR_OUT_OF_MEMORY;
        AIRY_MEMCPY(key_src, text, tl);
        key_src[tl] = '|';
        AIRY_MEMCPY(key_src + tl + 1, model_id, ml + 1);
        sha256_hex(key_src, exact_key);
        AIRY_FREE(key_src);
    }

    /* 同键覆盖：删除旧条目再插入（保持 LRU 语义） */
    if ((e = entry_find_exact(cache, exact_key)) != NULL) {
        uint64_t eh = cache_hash64(exact_key) % CACHE_HASH_BUCKETS;
        cache_entry_t *prev = NULL;
        cache_entry_t *cur = cache->buckets[eh];
        while (cur && cur != e) {
            prev = cur;
            cur = cur->next;
        }
        entry_unlink(cache, prev, e);
    }

    e = AIRY_CALLOC(1, sizeof(cache_entry_t));
    if (!e)
        return AIRY_ERR_OUT_OF_MEMORY;
    e->text = AIRY_STRDUP(text);
    e->response = AIRY_STRDUP(response);
    e->model_id = AIRY_STRDUP(model_id);
    if (!e->text || !e->response || !e->model_id) {
        AIRY_FREE(e->text);
        AIRY_FREE(e->response);
        AIRY_FREE(e->model_id);
        AIRY_FREE(e);
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    AIRY_STRNCPY_TERM(e->exact_key, exact_key, SHA256_HEX + 1);
    cache_id_gen(cache, e->cache_id);
    e->created_at = cache_now_ns();
    e->last_access = e->created_at;
    e->access_count = 0;
    e->ttl_ms = ttl_ms > 0 ? ttl_ms : cache->default_ttl_ms;
    e->bytes = strlen(text) + strlen(response) + strlen(model_id);

    h = cache_hash64(exact_key) % CACHE_HASH_BUCKETS;
    e->next = cache->buckets[h];
    cache->buckets[h] = e;
    cache->count++;
    cache->bytes += e->bytes;

    cache_evict(cache);

    if (out_cache_id)
        *out_cache_id = AIRY_STRDUP(e->cache_id);
    if (out_exact_key)
        *out_exact_key = AIRY_STRDUP(exact_key);
    return AIRY_SUCCESS;
}

int mem_cache_get(mem_cache_t *cache, const char *text, const char *model_id,
                  double threshold, int *out_hit, double *out_score,
                  char **out_cache_id, char **out_response)
{
    char exact_key[SHA256_HEX + 1];
    cache_entry_t *e;
    uint64_t now;
    double thr;

    if (out_hit) *out_hit = 0;
    if (out_score) *out_score = 0.0;
    if (out_cache_id) *out_cache_id = NULL;
    if (out_response) *out_response = NULL;
    if (!cache || !text || !model_id) {
        if (cache) cache->misses++;
        return AIRY_ERR_INVALID_PARAM;
    }

    now = cache_now_ns();
    thr = threshold > 0.0 && threshold <= 1.0 ? threshold : cache->semantic_threshold;

    {
        size_t tl = strlen(text), ml = strlen(model_id);
        char *key_src = AIRY_MALLOC(tl + ml + 2);
        if (!key_src)
            return AIRY_ERR_OUT_OF_MEMORY;
        AIRY_MEMCPY(key_src, text, tl);
        key_src[tl] = '|';
        AIRY_MEMCPY(key_src + tl + 1, model_id, ml + 1);
        sha256_hex(key_src, exact_key);
        AIRY_FREE(key_src);
    }

    /* L0 精确 */
    if ((e = entry_find_exact(cache, exact_key)) != NULL) {
        if (e->ttl_ms > 0 && now - e->created_at >= e->ttl_ms * 1000000ULL) {
            uint64_t eh = cache_hash64(exact_key) % CACHE_HASH_BUCKETS;
            cache_entry_t *prev = NULL;
            cache_entry_t *cur = cache->buckets[eh];
            while (cur && cur != e) {
                prev = cur;
                cur = cur->next;
            }
            entry_unlink(cache, prev, e);
            cache->misses++;
            return AIRY_SUCCESS;
        }
        e->access_count++;
        e->last_access = now;
        cache->hits++;
        if (out_hit) *out_hit = 1;
        if (out_score) *out_score = 1.0;
        if (out_cache_id) *out_cache_id = AIRY_STRDUP(e->cache_id);
        if (out_response) *out_response = AIRY_STRDUP(e->response);
        return AIRY_SUCCESS;
    }

    /* L1 语义（同模型候选） */
    {
        double best = 0.0;
        cache_entry_t *best_e = NULL;
        for (int i = 0; i < CACHE_HASH_BUCKETS && !best_e; i++) {
            for (cache_entry_t *cur = cache->buckets[i]; cur; cur = cur->next) {
                if (strcmp(cur->model_id, model_id) != 0)
                    continue;
                if (cur->ttl_ms > 0 && now - cur->created_at >= cur->ttl_ms * 1000000ULL)
                    continue; /* 过期条目不参与命中（后续淘汰清理） */
                double s = semantic_score(text, cur->text);
                if (s > best) {
                    best = s;
                    best_e = cur;
                }
            }
        }
        if (best_e && best >= thr) {
            best_e->access_count++;
            best_e->last_access = now;
            cache->hits++;
            if (out_hit) *out_hit = 1;
            if (out_score) *out_score = best;
            if (out_cache_id) *out_cache_id = AIRY_STRDUP(best_e->cache_id);
            if (out_response) *out_response = AIRY_STRDUP(best_e->response);
            return AIRY_SUCCESS;
        }
    }

    cache->misses++;
    return AIRY_SUCCESS;
}

int mem_cache_del(mem_cache_t *cache, const char *cache_id, int *out_deleted)
{
    if (out_deleted) *out_deleted = 0;
    if (!cache || !cache_id)
        return AIRY_ERR_INVALID_PARAM;

    for (int i = 0; i < CACHE_HASH_BUCKETS; i++) {
        cache_entry_t *prev = NULL;
        cache_entry_t *e = cache->buckets[i];
        while (e) {
            if (strcmp(e->cache_id, cache_id) == 0) {
                cache_entry_t *victim = e;
                e = e->next;
                entry_unlink(cache, prev, victim);
                if (out_deleted) *out_deleted = 1;
                return AIRY_SUCCESS;
            }
            prev = e;
            e = e->next;
        }
    }
    return AIRY_SUCCESS;
}

void mem_cache_stats(mem_cache_t *cache, mem_cache_stats_t *out)
{
    size_t total = 0;
    if (!cache || !out)
        return;
    for (int i = 0; i < CACHE_HASH_BUCKETS; i++) {
        for (cache_entry_t *e = cache->buckets[i]; e; e = e->next)
            total += e->bytes;
    }
    out->entries = cache->count;
    out->hits = cache->hits;
    out->misses = cache->misses;
    out->hit_rate = (cache->hits + cache->misses) > 0
                        ? (double)cache->hits / (double)(cache->hits + cache->misses)
                        : 0.0;
    out->evictions = cache->evictions;
    out->bytes = total;
}
