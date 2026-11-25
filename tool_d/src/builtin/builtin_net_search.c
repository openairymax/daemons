/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_net_search.c
 * @brief Built-in tool web_search: configurable JSON provider first,
 *        Bing / DuckDuckGo HTML scraping only as a fallback.
 *
 *        params: query (required), max_results (optional, capped at 8)
 *        output: title / URL / summary groups, line-separated
 *
 *        Provider configuration (environment, read per call):
 *          AIRY_WEB_SEARCH_ENDPOINT  JSON API URL template containing %s for
 *                                    the URL-encoded query; without %s the
 *                                    query is appended as ?q=/&q=
 *          AIRY_WEB_SEARCH_KEY       optional credential, sent both as
 *                                    "Authorization: Bearer" and "X-API-Key"
 *
 *        When a provider is configured but unreachable and the SERP
 *        fallback also fails, the tool returns an explicit offline error
 *        instead of a generic soft empty result.
 */

#include "builtin/builtin_net_internal.h"

#include "airy_prov/provider_transport.h"

#include "airy_regex.h"

#include "builtin/builtin_buf_internal.h"

#define BUILTIN_WEBSEARCH_MAX 8

static void builtin_url_encode(const char *in, char *out, size_t out_cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c && o + 3 < out_cap; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
            *c == '-' || *c == '_' || *c == '.' || *c == '~') {
            out[o++] = (char)*c;
        } else {
            out[o++] = '%';
            out[o++] = hex[*c >> 4];
            out[o++] = hex[*c & 0xF];
        }
    }
    out[o] = '\0';
}

static void builtin_html_unescape(char *s)
{
    char *r = s;
    while (*s) {
        if (*s == '&') {
            if (strncmp(s, "&amp;", 5) == 0) {
                *r++ = '&';
                s += 5;
                continue;
            }
            if (strncmp(s, "&lt;", 4) == 0) {
                *r++ = '<';
                s += 4;
                continue;
            }
            if (strncmp(s, "&gt;", 4) == 0) {
                *r++ = '>';
                s += 4;
                continue;
            }
            if (strncmp(s, "&quot;", 6) == 0) {
                *r++ = '"';
                s += 6;
                continue;
            }
            if (strncmp(s, "&#x27;", 6) == 0 || strncmp(s, "&#39;", 5) == 0) {
                *r++ = '\'';
                s += (strncmp(s, "&#x27;", 6) == 0) ? 6 : 5;
                continue;
            }
            if (strncmp(s, "&nbsp;", 6) == 0) {
                *r++ = ' ';
                s += 6;
                continue;
            }
        }
        *r++ = *s++;
    }
    *r = '\0';
}

/* 去标签并反转义实体。实体反转义必须作用于输出起点：旧实现把已移动到
 * 串尾的 s 传给 unescape，等于完全不反转义（标题里的 &amp; 原样输出）。 */
static void builtin_strip_html(char *s)
{
    char *start = s;
    char *r = s;
    while (*s) {
        if (*s == '<') {
            while (*s && *s != '>')
                s++;
            if (*s)
                s++;
            continue;
        }
        *r++ = *s++;
    }
    *r = '\0';
    builtin_html_unescape(start);
}

static void snip_take(const char *base, const regmatch_t *m, char *snip, size_t cap)
{
    size_t len = (size_t)(m->rm_eo - m->rm_so);
    if (len > cap - 1)
        len = cap - 1;
    AIRY_MEMCPY(snip, base + m->rm_so, len);
    snip[len] = '\0';
    builtin_strip_html(snip);
}

/* Generic search-result extraction: pull up to max entries from html using a
 * result regex + snippet regex, writing to buf (line-separated:
 * "[N] title\n    url\n    snippet\n"). */
static int web_search_extract(const char *html, const char *res_pattern, const char *snip_pattern,
                              int max, char *buf, size_t buf_cap, size_t *buf_len, int *count)
{
    regex_t re, re_s;
    if (regcomp(&re, res_pattern, REG_EXTENDED) != 0) return -1;
    int has_s = (regcomp(&re_s, snip_pattern, REG_EXTENDED) == 0);

    const char *cur = html;
    regmatch_t m[3];
    while (*count < max && regexec(&re, cur, 3, m, 0) == 0) {
        size_t url_len = (size_t)(m[1].rm_eo - m[1].rm_so);
        size_t title_len = (size_t)(m[2].rm_eo - m[2].rm_so);
        char *url = (char *)AIRY_MALLOC(url_len + 1);
        char *title = (char *)AIRY_MALLOC(title_len + 1);
        if (!url || !title) {
            AIRY_FREE(url);
            AIRY_FREE(title);
            break;
        }
        AIRY_MEMCPY(url, cur + m[1].rm_so, url_len);
        url[url_len] = '\0';
        AIRY_MEMCPY(title, cur + m[2].rm_so, title_len);
        title[title_len] = '\0';
        builtin_strip_html(title);

        char snippet[512] = "";
        if (has_s) {
            const char *sp = cur + m[0].rm_eo;
            /* 摘要片段可能不在结果块起始处（Bing 的 class="b_lineclamp…"、
             * DDG 的 result__snippet 都在标题之后），先按 class= 逐处探测，
             * 全部失败再对结果块整体试一次。 */
            const char *probe = strstr(sp, "class=");
            regmatch_t sm[2];
            while (probe) {
                if (regexec(&re_s, probe, 2, sm, 0) == 0) {
                    snip_take(probe, &sm[1], snippet, sizeof(snippet));
                    break;
                }
                probe = strstr(probe + 6, "class=");
            }
            if (!probe && regexec(&re_s, sp, 2, sm, 0) == 0)
                snip_take(sp, &sm[1], snippet, sizeof(snippet));
        }

        size_t need = title_len + url_len + strlen(snippet) + 16;
        if (*buf_len + need >= buf_cap) {
            builtin_append_trunc_mark(buf, buf_cap, *buf_len, "\n[search truncated]");
            AIRY_FREE(url);
            AIRY_FREE(title);
            break;
        }
        int w = snprintf(buf + *buf_len, buf_cap - *buf_len, "[%d] %s\n    %s\n    %s\n",
                         *count + 1, title, url, snippet);
        if (w > 0)
            *buf_len += (size_t)w;
        (*count)++;
        AIRY_FREE(url);
        AIRY_FREE(title);
        cur += m[0].rm_eo;
    }
    regfree(&re);
    if (has_s)
        regfree(&re_s);
    return 0;
}

/* ---- 中文查询兜底（2026-08-20）----
 * Bing 对「无空格长中文查询」分词退化：返回汉字字典/拼音词条而非网页
 * 结果（实测 "宇树科技 上市 时间 股票代码" 返回"宇（汉语汉字）_百度百科"
 * 等，用户会话中 agent 报"一直返回字典结果"即此现象）。行业级搜索依赖
 * 查询改写；无分词器时用两条启发式兜底：
 *   1) 移除高频查询意图词（时间/股票代码/是什么…），保留核心实体；
 *   2) 对剩余连续中文每 4 字插入空格（"宇树科技上市" → "宇树科技 上市"）。
 * 组合实测可将退化查询恢复为正常新闻结果。改写仅在检测到退化时启用，
 * 正常查询零开销。 */

/* 标题是否命中字典特征（Bing 分词退化时的典型产物）。 */
static int web_search_is_dict_hit(const char *title)
{
    static const char *const dict_markers[] = {
        "汉字", "拼音", "部首", "笔顺", "组词", "词典", "字典",
        "汉语", "意思", "释义", "笔画",
    };
    for (size_t i = 0; i < sizeof(dict_markers) / sizeof(dict_markers[0]); i++) {
        if (strstr(title, dict_markers[i])) return 1;
    }
    return 0;
}

/* 扫描提取结果（"[N] title\n    url\n    snippet\n"），条目 ≥2 且过半
 * 命中字典特征 → 判定该查询分词退化。 */
static int web_search_results_degraded(const char *buf)
{
    int total = 0, dict = 0;
    const char *p = buf;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        const char *title = strchr(p, ']');
        if (title && title < nl) {
            total++;
            if (web_search_is_dict_hit(title + 1))
                dict++;
        }
        p = nl + 1;
    }
    return (total >= 2 && dict >= (total + 1) / 2) ? 1 : 0;
}

/* 对连续非 ASCII 段（中文等，≥5 字符）每 4 字符插入空格，修复 Bing
 * 对长中文串的分词退化。原地改写（s 是调用方的可写缓冲）。 */
static void web_search_space_cjk(char *s, size_t cap)
{
    char tmp[2048];
    size_t o = 0;
    size_t lim = (cap < sizeof(tmp)) ? cap : sizeof(tmp);
    size_t run = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 2 < lim;) {
        if (*p < 0x80) {
            tmp[o++] = (char)*p++;
            run = 0;
            continue;
        }
        if (*p < 0xC0) { /* 续字节：与所属首字节同属一个码点，不计入 run */
            tmp[o++] = (char)*p++;
            continue;
        }
        tmp[o++] = (char)*p++;
        /* run 满 4 且后面仍是非 ASCII：说明该串至少 5 个字，插入空格 */
        if (++run % 4 == 0 && *p >= 0x80 && o + 1 < lim)
            tmp[o++] = ' ';
    }
    tmp[o] = '\0';
    if (o < cap)
        snprintf(s, cap, "%s", tmp);
}

/* 改写退化查询：移除高频意图词（触发 Bing 词典分词的词）→ 连续中文插
 * 空格。改写后与原查询相同（无可改之处）返回 0。 */
static int web_search_rewrite_query(const char *in, char *out, size_t cap)
{
    static const char *const intent_words[] = {
        "股票代码", "时间", "是什么", "怎么回事", "为什么", "什么时候",
        "多久", "多少", "哪个", "哪里", "如何", "怎么", "是否", "有没有",
    };
    size_t o = 0;
    const char *p = in;
    while (*p && o + 1 < cap) {
        const char *before = p;
        for (size_t i = 0; i < sizeof(intent_words) / sizeof(intent_words[0]); i++) {
            size_t wl = strlen(intent_words[i]);
            if (strncmp(p, intent_words[i], wl) == 0) {
                p += wl;
                break;
            }
        }
        if (p == before)
            out[o++] = *p++;
    }
    out[o] = '\0';
    web_search_space_cjk(out, cap);
    return (strcmp(in, out) != 0) ? 1 : 0;
}

/* 搜索后端描述表：URL 模板 + 请求预算 + 结果/摘要正则。两家 SERP 的
 * 差异全部收敛在此，抓取与解析骨架只有一份。
 *
 * Bing 排首位：多数网络下响应快。--max-time 12s（shell 15s）是为串联
 * 预算服务——Bing 失败后还要试 DDG，两段必须压在 30s RPC 内（原
 * 20s+8s 会超）。
 *
 * POSIX ERE（glibc regcomp REG_EXTENDED）不支持非贪婪 `.*?`——它会被解析
 * 为贪婪 `.*` 加可选字面 `?`，一次匹配就吞掉整页所有结果（实测 Bing
 * SERP 10 条被压成 1 条巨型 blob）。标题/摘要文本用
 * `[^<]*(<(/?(strong|b|em))[^>]*>[^<]*)*` 跨强调标签提取（关键词包在
 * <strong> 中），标签迭代仅限强调标签，不会吞掉下一结果的
 * `<h2>/<a>/</a>`，从而逐条干净截断。 */
#define WEB_SEARCH_UA "Mozilla/5.0 (compatible; AirymaxRT/0.1.2 web_search)"
#define WEB_SEARCH_TXT "[^<]*(<(/?(strong|b|em))[^>]*>[^<]*)*"

typedef struct {
    const char *url_fmt;
    int shell_ms;
    const char *res_re;
    const char *snip_re;
} web_src_t;

static const web_src_t WEB_SRC_BING = {
    "https://www.bing.com/search?q=%s",
    15000,
    "<h2[^>]*><a[^>]*href=\"([^\"]+)\"[^>]*>(" WEB_SEARCH_TXT ")</a></h2>",
    "class=\"b_lineclamp[^\"]*\"[^>]*>(" WEB_SEARCH_TXT ")</p>",
};

static const web_src_t WEB_SRC_DDG = {
    "https://html.duckduckgo.com/html/?q=%s",
    10000,
    "class=\"result__a\" href=\"([^\"]+)\"[^>]*>(" WEB_SEARCH_TXT ")</a>",
    "class=\"result__snippet\"[^>]*>(" WEB_SEARCH_TXT ")</a>",
};

/* ---------------------------------------------------------------------------
 * 可配置 JSON provider（S-P1-06）
 * ------------------------------------------------------------------------- */

static const char *json_text(const cJSON *item, const char *key)
{
    const cJSON *n = cJSON_GetObjectItem(item, key);
    return (cJSON_IsString(n) && n->valuestring && n->valuestring[0]) ? n->valuestring : NULL;
}

static const char *pick_str(const cJSON *it, const char *a, const char *b, const char *c)
{
    const char *s = json_text(it, a);
    if (!s && b)
        s = json_text(it, b);
    if (!s && c)
        s = json_text(it, c);
    return s;
}

/* 在常见响应外壳里找结果数组：Bing(webPages.value)、Brave(web.results)、
 * Serper(organic_results)、Tavily(results) 以及通用 results/items/data。 */
static const cJSON *find_items(const cJSON *node, int depth)
{
    static const char *const keys[] = {"results", "items", "value",   "data", "webPages",
                                       "web",     "organic_results", "entries"};
    if (cJSON_IsArray(node))
        return node;
    if (!cJSON_IsObject(node) || depth > 2)
        return NULL;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *n = cJSON_GetObjectItem(node, keys[i]);
        if (cJSON_IsArray(n))
            return n;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *r = find_items(cJSON_GetObjectItem(node, keys[i]), depth + 1);
        if (r)
            return r;
    }
    return NULL;
}

int net_search_json(const char *json, int max, char *buf, size_t cap, size_t *len)
{
    if (!json || !buf || !len || max <= 0 || cap == 0)
        return 0;
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return 0;

    const cJSON *items = find_items(root, 0);
    int count = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        if (count >= max)
            break;
        const char *url = pick_str(it, "url", "link", "href");
        const char *title = pick_str(it, "title", "name", "heading");
        const char *snip = pick_str(it, "snippet", "description", "content");
        if (!snip)
            snip = pick_str(it, "text", "summary", NULL);
        if (!url || !title)
            continue;
        size_t need = strlen(title) + strlen(url) + (snip ? strlen(snip) : 0) + 16;
        if (*len + need >= cap) {
            builtin_append_trunc_mark(buf, cap, *len, "\n[search truncated]");
            break;
        }
        int w = snprintf(buf + *len, cap - *len, "[%d] %s\n    %s\n    %s\n", count + 1, title,
                         url, snip ? snip : "");
        if (w > 0) {
            *len += (size_t)w;
            count++;
        }
    }
    cJSON_Delete(root);
    return count;
}

/* endpoint 只允许 http(s) 且不得含易混字符（配置卫生）。 */
static int ep_check(const char *ep)
{
    if (!ep || !ep[0])
        return -1;
    if (strpbrk(ep, "'\"`$\\") != NULL)
        return -1;
    return (strncmp(ep, "http://", 7) == 0 || strncmp(ep, "https://", 8) == 0) ? 0 : -1;
}

/* key 只允许 token 安全字符，避免写入请求头时形成注入面。 */
static int key_check(const char *k)
{
    for (const unsigned char *p = (const unsigned char *)k; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))
            continue;
        if (strchr("._~+/=-", *p) != NULL)
            continue;
        return -1;
    }
    return 0;
}

static int search_cfg_bad(const char *ep, const char *key)
{
    return (ep_check(ep) != 0 || (key[0] && key_check(key) != 0)) ? 1 : 0;
}

static int provider_url(const char *ep, const char *enc, char *url, size_t cap)
{
    int w;
    if (strstr(ep, "%s") != NULL)
        w = snprintf(url, cap, ep, enc);
    else
        w = snprintf(url, cap, "%s%sq=%s", ep, strchr(ep, '?') ? "&" : "?", enc);
    return (w > 0 && (size_t)w < cap) ? 0 : -1;
}

/* 返回 0=拿到结果，-1=网络/解析失败（调用方转入 SERP 兜底）。 */
static int provider_run(const char *query, int max, char *buf, size_t *len, int *count)
{
    char ep[2048], key[512], enc[2048], url[4096];
    char auth[640], apikey[640];

    if (!net_cfg_provider(ep, sizeof(ep), key, sizeof(key)))
        return -1;
    if (search_cfg_bad(ep, key))
        return -1;
    builtin_url_encode(query, enc, sizeof(enc));
    if (provider_url(ep, enc, url, sizeof(url)) != 0)
        return -1;

    struct curl_slist *hdr = curl_slist_append(NULL, "User-Agent: " WEB_SEARCH_UA);
    hdr = curl_slist_append(hdr, "Accept: application/json");
    if (key[0]) {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);
        snprintf(apikey, sizeof(apikey), "X-API-Key: %s", key);
        hdr = curl_slist_append(hdr, auth);
        hdr = curl_slist_append(hdr, apikey);
    }

    net_http_req_t req;
    req.url = url;
    req.headers = hdr;
    req.timeout_ms = 18000;
    net_http_resp_t rsp = {0};
    int rc = net_http_get(&req, &rsp);
    if (hdr)
        curl_slist_free_all(hdr);
    if (rc != 0 || rsp.exit_code != 0 || !rsp.body) {
        AIRY_FREE(rsp.body);
        return -1;
    }
    int got = net_search_json(rsp.body, max, buf, BUILTIN_OUTPUT_CAP, len);
    AIRY_FREE(rsp.body);
    *count = got;
    return (got > 0) ? 0 : -1;
}

static int web_search_via(const web_src_t *src, const char *query, int max_results, char *buf,
                          size_t buf_cap, size_t *buf_len, int *count)
{
    char enc[2048], url[4096];
    builtin_url_encode(query, enc, sizeof(enc));
    snprintf(url, sizeof(url), src->url_fmt, enc);
    struct curl_slist *hdr = curl_slist_append(NULL, "User-Agent: " WEB_SEARCH_UA);

    net_http_req_t req;
    req.url = url;
    req.headers = hdr;
    req.timeout_ms = (uint32_t)src->shell_ms;
    net_http_resp_t rsp = {0};
    int rc = net_http_get(&req, &rsp);
    if (hdr)
        curl_slist_free_all(hdr);
    if (rc != 0 || rsp.exit_code != 0 || !rsp.body) {
        AIRY_FREE(rsp.body);
        return -1;
    }
    int rc2 = web_search_extract(rsp.body, src->res_re, src->snip_re, max_results, buf, buf_cap,
                                 buf_len, count);
    AIRY_FREE(rsp.body);
    return rc2;
}

/* 搜索编排：可配置 provider → Bing → DuckDuckGo → 退化查询改写重试；
 * SERP 仅作为 provider 未配置或失败时的兜底。返回最终条目数。 */
static int search_run(const char *query, int max_results, uint64_t deadline, char *buf,
                      size_t *buf_len, int *count)
{
    if (net_cfg_provider(NULL, 0, NULL, 0)) {
        if (provider_run(query, max_results, buf, buf_len, count) == 0 && *count > 0)
            return *count;
        *buf_len = 0; /* provider 失败/无结果：清空后走 SERP 兜底 */
        *count = 0;
        if (builtin_deadline_hit(deadline))
            return 0;
    }

    int got = (web_search_via(&WEB_SRC_BING, query, max_results, buf, BUILTIN_OUTPUT_CAP, buf_len,
                              count) == 0 &&
               *count > 0);
    if (!got) {
        if (!builtin_deadline_hit(deadline))
            web_search_via(&WEB_SRC_DDG, query, max_results, buf, BUILTIN_OUTPUT_CAP, buf_len,
                           count);
        return *count;
    }
    if (!web_search_results_degraded(buf) || builtin_deadline_hit(deadline))
        return *count;

    /* Bing 命中但结果是汉字字典/词条（中文长查询分词退化）：改写查询后
     * 重试；改写仍退化/失败则清空走 DDG 兜底（2026-08-20）。 */
    char rq[2048];
    int rewrote = web_search_rewrite_query(query, rq, sizeof(rq));
    *buf_len = 0;
    *count = 0;
    if (rewrote && web_search_via(&WEB_SRC_BING, rq, max_results, buf, BUILTIN_OUTPUT_CAP,
                                  buf_len, count) == 0 &&
        *count > 0 && !web_search_results_degraded(buf))
        return *count;
    *buf_len = 0;
    *count = 0;
    web_search_via(&WEB_SRC_DDG, query, max_results, buf, BUILTIN_OUTPUT_CAP, buf_len, count);
    return *count;
}

int web_search_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });
    cJSON *q = cJSON_GetObjectItem(root, "query");
    if (!cJSON_IsString(q) || !q->valuestring || !q->valuestring[0])
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "Missing string parameter: query");
    cJSON *mr = cJSON_GetObjectItem(root, "max_results");
    int max_results =
        (cJSON_IsNumber(mr) && mr->valueint > 0) ? mr->valueint : BUILTIN_WEBSEARCH_MAX;
    if (max_results > BUILTIN_WEBSEARCH_MAX)
        max_results = BUILTIN_WEBSEARCH_MAX;

    char ep[2048], key[512];
    int has_provider = net_cfg_provider(ep, sizeof(ep), key, sizeof(key));
    if (has_provider && search_cfg_bad(ep, key))
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "Invalid AIRY_WEB_SEARCH_ENDPOINT/KEY (endpoint must be http(s) "
                          "without quotes; key must be token-safe)");

    char *buf = (char *)AIRY_CALLOC(BUILTIN_OUTPUT_CAP, 1);
    if (!buf) return btool_fail(res, AIRY_ERR_OUT_OF_MEMORY, "Out of memory");
    size_t buf_len = 0;
    int count = 0;

    /* Primary path: the configured provider (when present). SERP scraping is
     * only a fallback for networks where the provider is blocked or absent.
     * Each endpoint is given a tight --max-time so an unreachable search
     * provider cannot stall the IPC round trip beyond the agent's 30s
     * timeout. The stage budget (timeout_ms) gates the fallbacks so the sum
     * of retries stays within the executor's per-tool budget. */
    (void)search_run(q->valuestring, max_results, builtin_deadline_ms(timeout_ms), buf, &buf_len,
                     &count);

    if (count == 0) {
        AIRY_FREE(buf);
        res->exit_code = 1;
        if (has_provider)
            return btool_fail(res, AIRY_ERR_IO,
                              "web_search offline: configured provider unreachable and SERP "
                              "fallback failed");
        return btool_fail(res, AIRY_ERR_IO,
                          "web_search offline: no provider configured (set "
                          "AIRY_WEB_SEARCH_ENDPOINT) and SERP fallback failed");
    }
    return btool_okout(res, buf);
}
