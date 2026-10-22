// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_net.c
 * @brief Built-in tool network domain: web_fetch page fetch and web_search
 *        web-search tool implementations.
 */

#include "network_common.h"

#include "airy_regex.h"

#include "builtin/builtin_buf_internal.h"
#include "builtin/builtin_exec_internal.h"
#include "builtin/builtin_time_internal.h"

/* ============================================================================
 * web_fetch: fetch web page content over the network
 *
 * Main path: curl child process (production-grade HTTPS/TLS + redirects +
 * Content-Length parsing, sharing builtin_shell_run's timeout infrastructure
 * with shell_run).
 * Fallback path: when curl is unavailable, fall back to the network_common
 * layer (http: only — the abstraction does not implement TLS yet, https needs
 * curl).
 * ============================================================================ */

typedef struct {
    char scheme[8]; /* "http" / "https" */
    char host[256];
    int port;
    char path[2048];
} builtin_url_t;

/**
 * @brief Parse a URL into scheme/host/port/path (http/https only)
 * @return 0 on success, -1 on invalid URL
 */
static int builtin_parse_url(const char *url, builtin_url_t *u)
{
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end || scheme_end == url) {
        return -1;
    }
    size_t slen = (size_t)(scheme_end - url);
    if (slen >= sizeof(u->scheme)) {
        return -1;
    }
    AIRY_MEMCPY(u->scheme, url, slen);
    u->scheme[slen] = '\0';
    if (strcmp(u->scheme, "http") != 0 && strcmp(u->scheme, "https") != 0) {
        return -1;
    }

    const char *host_start = scheme_end + 3;
    const char *p = host_start;
    while (*p && *p != ':' && *p != '/' && *p != '?') {
        p++;
    }
    size_t hlen = (size_t)(p - host_start);
    if (hlen == 0 || hlen >= sizeof(u->host)) {
        return -1;
    }
    AIRY_MEMCPY(u->host, host_start, hlen);
    u->host[hlen] = '\0';

    u->port = (strcmp(u->scheme, "https") == 0) ? 443 : 80;
    const char *path_start = p;
    if (*p == ':') {
        const char *port_start = p + 1;
        const char *q = port_start;
        while (*q && *q != '/' && *q != '?') {
            if (*q < '0' || *q > '9') {
                return -1;
            }
            q++;
        }
        if (q == port_start) {
            return -1;
        }
        long port = strtol(port_start, NULL, 10);
        if (port <= 0 || port > 65535) {
            return -1;
        }
        u->port = (int)port;
        path_start = q;
    }
    if (*path_start == '\0') {
        u->path[0] = '/';
        u->path[1] = '\0';
    } else {
        if (strlen(path_start) >= sizeof(u->path)) {
            return -1;
        }
        AIRY_MEMCPY(u->path, path_start, strlen(path_start) + 1);
    }
    return 0;
}

#ifndef _WIN32
/**
 * @brief Fallback path: HTTP GET via the network_common layer (http: only)
 */
static int web_fetch_via_network(const builtin_url_t *u, tool_result_t *res)
{
    network_config_t cfg = network_create_default_config();
    cfg.host = u->host;
    cfg.port = u->port;
    cfg.timeout_ms = 20000;
    cfg.read_timeout_ms = 15000;
    cfg.write_timeout_ms = 15000;

    network_connection_t *conn = network_connection_create(&cfg);
    if (!conn)
        return btool_fail(res, AIRY_ERR_OUT_OF_MEMORY,
                          "Out of memory creating network connection");
    airy_err_t e = network_connect(conn);
    if (e != AIRY_SUCCESS) {
        btool_fail(res, AIRY_ERR_IO, "Connect to %s:%d failed: %s", u->host, u->port,
                   network_get_error_message(conn));
        network_connection_destroy(conn);
        return AIRY_ERR_IO;
    }

    network_http_response_t resp;
    AIRY_MEMSET(&resp, 0, sizeof(resp));
    e = network_http_get(conn, u->path, &resp);
    network_connection_destroy(conn);
    if (e != AIRY_SUCCESS) {
        btool_fail(res, AIRY_ERR_IO, "HTTP GET '%s' failed: %s", u->path,
                   resp.error_message ? resp.error_message : "unknown error");
        network_http_response_free(&resp);
        return AIRY_ERR_IO;
    }

    if (resp.body && resp.body_len > 0) {
        res->output = (char *)AIRY_MALLOC(resp.body_len + 1);
        if (!res->output) {
            network_http_response_free(&resp);
            res->error = AIRY_STRDUP("Out of memory allocating response body");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        AIRY_MEMCPY(res->output, resp.body, resp.body_len);
        res->output[resp.body_len] = '\0';
    } else {
        res->output = AIRY_STRDUP("");
    }
    res->success = (resp.status_code >= 200 && resp.status_code < 400) ? 1 : 0;
    res->exit_code = resp.status_code;
    if (!res->success)
        btool_fail(res, AIRY_ERR_IO, "HTTP error status %d", resp.status_code);
    network_http_response_free(&resp);
    return AIRY_OK;
}
#endif

int web_fetch_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });
    cJSON *url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(url) || !url->valuestring || !url->valuestring[0])
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "Missing string parameter: url");
    const char *url_str = url->valuestring;

    if (strpbrk(url_str, "'\"`$\\") != NULL)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "URL contains unsafe characters (', \", `, $, \\)");
    builtin_url_t u;
    if (builtin_parse_url(url_str, &u) != 0)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "Invalid URL (only http:// and https:// are supported)");

    /* Main path: curl child process (curl.exe ships with Windows 10+).
     * -sS silences progress but keeps errors; -L follows redirects;
     * --max-time prevents hangs; -w appends a status marker (single quotes let
     * curl interpret \n as newline); -A declares the UA.
     *
     * 超时预算（2026-08-19）：CLI 侧工具 RPC 超时为 30s（cli_chat.c
     * CLI_TOOL_RPC_TIMEOUT_MS），curl --max-time 若同样 30s，慢网页会先
     * 撞 RPC 超时（实测 rc=-31 超时）。curl 降到 20s、shell_run 降到
     * 25s，给 RPC 返回留出余量——20s 足够绝大多数页面（大文件/流式站点
     * 快速失败，不拖垮整个工具轮）。 */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
             "curl -sSL --max-time 20 -A \"AirymaxRT/0.1.2 web_fetch\" "
             "-w '\\n__AIRY_STATUS__:%%{http_code}' '%s'",
             url_str);

    char *out = NULL;
    int exit_code = -1;

    int rc = builtin_shell_run(cmd, NULL, &out, &exit_code, timeout_ms ? timeout_ms : 25000, NULL,
                               NULL);
    if (rc != 0)
        return btool_fail(res, AIRY_ERR_EXEC_FAIL,
                          "Failed to execute web fetch (pipe/process creation failed)");

    if (exit_code == 127 || (out && strstr(out, "command not found"))) {
        AIRY_FREE(out);
#ifndef _WIN32
        /* Fallback: plain-HTTP via the built-in network layer (no TLS). */
        if (strcmp(u.scheme, "http") == 0)
            return web_fetch_via_network(&u, res);
#endif
        return btool_fail(res, AIRY_ERR_NOT_SUPPORTED,
                          "curl is not available and %s:// requires curl (TLS not supported by "
                          "the built-in network layer)",
                          u.scheme);
    }

    long http_status = 0;
    char *mark = out ? strstr(out, "\n__AIRY_STATUS__:") : NULL;
    if (mark) {
        *mark = '\0';
        http_status = strtol(mark + strlen("\n__AIRY_STATUS__:"), NULL, 10);
    }
    res->output = out ? out : AIRY_STRDUP("");

    if (http_status >= 400) {
        btool_fail(res, AIRY_ERR_IO, "HTTP error status %ld", http_status);
        res->success = 0;
        res->exit_code = (int)http_status;
    } else if (http_status > 0) {
        res->success = 1;
        res->exit_code = 0;
    } else {
        btool_fail(res, AIRY_ERR_IO, "Web fetch failed (curl exit %d): %s", exit_code,
                   out ? out : "");
        AIRY_FREE(res->output);
        res->output = AIRY_STRDUP("");
        res->success = 0;
        res->exit_code = exit_code;
    }
    return AIRY_OK;
}

/* ============================================================================
 * web_search: Bing / DuckDuckGo HTML search (modeled on Atom Code WebSearchTool)
 *   params: query (required), max_results (optional 8)
 *   output: title / URL / summary groups, line-separated
 * ============================================================================ */

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

static void builtin_strip_html(char *s)
{
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
    builtin_html_unescape(s);
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
    if (regcomp(&re, res_pattern, REG_EXTENDED) != 0)
        return -1;
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
        if (strstr(title, dict_markers[i]))
            return 1;
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
        if (!nl)
            break;
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

/* 搜索后端描述表：URL 模板 + curl 超时 + 结果/摘要正则。两家 SERP 的
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
    int curl_s;
    int shell_ms;
    const char *res_re;
    const char *snip_re;
} web_src_t;

static const web_src_t WEB_SRC_BING = {
    "https://www.bing.com/search?q=%s",
    12,
    15000,
    "<h2[^>]*><a[^>]*href=\"([^\"]+)\"[^>]*>(" WEB_SEARCH_TXT ")</a></h2>",
    "class=\"b_lineclamp[^\"]*\"[^>]*>(" WEB_SEARCH_TXT ")</p>",
};

static const web_src_t WEB_SRC_DDG = {
    "https://html.duckduckgo.com/html/?q=%s",
    8,
    10000,
    "class=\"result__a\" href=\"([^\"]+)\"[^>]*>(" WEB_SEARCH_TXT ")</a>",
    "class=\"result__snippet\"[^>]*>(" WEB_SEARCH_TXT ")</a>",
};

static int web_search_via(const web_src_t *src, const char *query, int max_results, char *buf,
                          size_t buf_cap, size_t *buf_len, int *count)
{
    char enc[2048], url[4096], cmd[8192];
    builtin_url_encode(query, enc, sizeof(enc));
    snprintf(url, sizeof(url), src->url_fmt, enc);
    snprintf(cmd, sizeof(cmd), "curl -sSL --max-time %d -A \"%s\" '%s'", src->curl_s,
             WEB_SEARCH_UA, url);
    char *out = NULL;
    int exit_code = -1;
    if (builtin_shell_run(cmd, NULL, &out, &exit_code, src->shell_ms, NULL, NULL) != 0 || !out ||
        exit_code != 0 || strstr(out, "command not found")) {
        AIRY_FREE(out);
        return -1;
    }
    int rc = web_search_extract(out, src->res_re, src->snip_re, max_results, buf, buf_cap,
                                buf_len, count);
    AIRY_FREE(out);
    return rc;
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

    char *buf = (char *)AIRY_CALLOC(BUILTIN_OUTPUT_CAP, 1);
    if (!buf)
        return btool_fail(res, AIRY_ERR_OUT_OF_MEMORY, "Out of memory");
    size_t buf_len = 0;
    int count = 0;

    /* Primary path: Bing (fast on most networks), then DuckDuckGo as a
     * fallback for networks where Bing is blocked. Each endpoint is given a
     * tight --max-time so an unreachable search provider cannot stall the
     * IPC round trip beyond the agent's 30s timeout. The stage budget
     * (timeout_ms) gates the fallbacks so the sum of retries stays within
     * the executor's per-tool budget. */
    uint64_t deadline = builtin_deadline_ms(timeout_ms);
    int got = (web_search_via(&WEB_SRC_BING, q->valuestring, max_results, buf, BUILTIN_OUTPUT_CAP,
                              &buf_len, &count) == 0 &&
               count > 0);
    if (!got && !builtin_deadline_hit(deadline)) {
        web_search_via(&WEB_SRC_DDG, q->valuestring, max_results, buf, BUILTIN_OUTPUT_CAP, &buf_len,
                       &count);
    } else if (got && web_search_results_degraded(buf) && !builtin_deadline_hit(deadline)) {
        /* Bing 命中但结果是汉字字典/词条（中文长查询分词退化）：改写查询
         * 后重试；改写仍退化/失败则清空走 DDG 兜底（2026-08-20）。 */
        char rq[2048];
        int rewrote = web_search_rewrite_query(q->valuestring, rq, sizeof(rq));
        buf_len = 0;
        count = 0;
        if (rewrote && web_search_via(&WEB_SRC_BING, rq, max_results, buf, BUILTIN_OUTPUT_CAP,
                                      &buf_len, &count) == 0 &&
            count > 0 && !web_search_results_degraded(buf)) {
            /* 改写后的查询返回正常结果，采用 */
        } else {
            buf_len = 0;
            count = 0;
            web_search_via(&WEB_SRC_DDG, q->valuestring, max_results, buf, BUILTIN_OUTPUT_CAP,
                           &buf_len, &count);
        }
    }

    if (count == 0) {
        btool_soft(res, "No web results for query: %s (Bing & DuckDuckGo unreachable)",
                   q->valuestring);
        AIRY_FREE(buf);
        return AIRY_OK;
    }
    return btool_okout(res, buf);
}
