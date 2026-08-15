// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_net.c
 * @brief Built-in tool network domain: web_fetch page fetch and web_search
 *        web-search tool implementations.
 */

#include "airy_memory.h"
#include "error.h"

#include "builtin.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <cjson_helpers.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network_common.h"

#ifndef _WIN32
#include <regex.h>
#endif

#include "tool_builtin_internal.h"

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
    __builtin_memcpy(u->scheme, url, slen);
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
    __builtin_memcpy(u->host, host_start, hlen);
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
        __builtin_memcpy(u->path, path_start, strlen(path_start) + 1);
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
    if (!conn) {
        res->error = AIRY_STRDUP("Out of memory creating network connection");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    airy_err_t e = network_connect(conn);
    if (e != AIRY_SUCCESS) {
        char err[512];
        snprintf(err, sizeof(err), "Connect to %s:%d failed: %s", u->host, u->port,
                 network_get_error_message(conn));
        network_connection_destroy(conn);
        res->error = AIRY_STRDUP(err);
        return AIRY_ERR_IO;
    }

    network_http_response_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    e = network_http_get(conn, u->path, &resp);
    network_connection_destroy(conn);
    if (e != AIRY_SUCCESS) {
        char err[512];
        snprintf(err, sizeof(err), "HTTP GET '%s' failed: %s", u->path,
                 resp.error_message ? resp.error_message : "unknown error");
        network_http_response_free(&resp);
        res->error = AIRY_STRDUP(err);
        return AIRY_ERR_IO;
    }

    if (resp.body && resp.body_len > 0) {
        res->output = (char *)AIRY_MALLOC(resp.body_len + 1);
        if (!res->output) {
            network_http_response_free(&resp);
            res->error = AIRY_STRDUP("Out of memory allocating response body");
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        __builtin_memcpy(res->output, resp.body, resp.body_len);
        res->output[resp.body_len] = '\0';
    } else {
        res->output = AIRY_STRDUP("");
    }
    res->success = (resp.status_code >= 200 && resp.status_code < 400) ? 1 : 0;
    res->exit_code = resp.status_code;
    if (!res->success) {
        char err[256];
        snprintf(err, sizeof(err), "HTTP error status %d", resp.status_code);
        res->error = AIRY_STRDUP(err);
    }
    network_http_response_free(&resp);
    return AIRY_OK;
}
#endif

int web_fetch_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(url) || !url->valuestring || !url->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: url");
        return AIRY_ERR_INVALID_PARAM;
    }
    const char *url_str = url->valuestring;

    if (strpbrk(url_str, "'\"`$\\") != NULL) {
        res->error = AIRY_STRDUP("URL contains unsafe characters (', \", `, $, \\)");
        return AIRY_ERR_INVALID_PARAM;
    }
    builtin_url_t u;
    if (builtin_parse_url(url_str, &u) != 0) {
        res->error = AIRY_STRDUP("Invalid URL (only http:// and https:// are supported)");
        return AIRY_ERR_INVALID_PARAM;
    }

#ifndef _WIN32
    /* Main path: curl child process.
     * -sS silences progress but keeps errors; -L follows redirects;
     * --max-time prevents hangs; -w appends a status marker (single quotes let
     * curl interpret \n as newline); -A declares the UA */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
             "curl -sSL --max-time 30 -A \"AirymaxRT/0.1.2 web_fetch\" "
             "-w '\\n__AIRY_STATUS__:%%{http_code}' '%s'",
             url_str);

    char *out = NULL;
    int exit_code = -1;

    int rc = builtin_shell_run(cmd, &out, &exit_code, 45000, NULL, NULL);
    if (rc != 0) {
        res->error = AIRY_STRDUP("Failed to execute web fetch (fork/pipe failed)");
        return AIRY_ERR_EXEC_FAIL;
    }

    if (exit_code == 127 || (out && strstr(out, "command not found"))) {
        AIRY_FREE(out);
        if (strcmp(u.scheme, "http") != 0) {
            char err[512];
            snprintf(err, sizeof(err),
                     "curl is not available and https:// requires curl (TLS not "
                     "supported by the built-in network layer)");
            res->error = AIRY_STRDUP(err);
            return AIRY_ERR_NOT_SUPPORTED;
        }
        return web_fetch_via_network(&u, res);
    }

    long http_status = 0;
    char *mark = out ? strstr(out, "\n__AIRY_STATUS__:") : NULL;
    if (mark) {
        *mark = '\0';
        http_status = strtol(mark + strlen("\n__AIRY_STATUS__:"), NULL, 10);
    }
    res->output = out ? out : AIRY_STRDUP("");

    if (http_status >= 400) {
        char err[256];
        snprintf(err, sizeof(err), "HTTP error status %ld", http_status);
        res->error = AIRY_STRDUP(err);
        res->success = 0;
        res->exit_code = (int)http_status;
    } else if (http_status > 0) {
        res->success = 1;
        res->exit_code = 0;
    } else {

        char err[512];
        snprintf(err, sizeof(err), "Web fetch failed (curl exit %d): %s", exit_code,
                 out ? out : "");
        AIRY_FREE(res->output);
        res->output = AIRY_STRDUP("");
        res->error = AIRY_STRDUP(err);
        res->success = 0;
        res->exit_code = exit_code;
    }
    return AIRY_OK;
#else
    res->error = AIRY_STRDUP("web_fetch is not supported on this platform");
    return AIRY_ERR_NOT_SUPPORTED;
#endif
}

#ifndef _WIN32
/* ============================================================================
 * web_search: DuckDuckGo HTML search (modeled on Atom Code WebSearchTool)
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
        __builtin_memcpy(url, cur + m[1].rm_so, url_len);
        url[url_len] = '\0';
        __builtin_memcpy(title, cur + m[2].rm_so, title_len);
        title[title_len] = '\0';
        builtin_strip_html(title);

        char snippet[512] = "";
        if (has_s) {
            const char *sp = cur + m[0].rm_eo;
            regmatch_t sm[2];

            const char *probe = sp;
            while (probe && *probe && (probe = strstr(probe, "class=")) != NULL) {
                if (regexec(&re_s, probe, 2, sm, 0) == 0) {
                    size_t slen = (size_t)(sm[1].rm_eo - sm[1].rm_so);
                    if (slen > sizeof(snippet) - 1)
                        slen = sizeof(snippet) - 1;
                    __builtin_memcpy(snippet, probe + sm[1].rm_so, slen);
                    snippet[slen] = '\0';
                    builtin_strip_html(snippet);
                    break;
                }
                probe += 6;
            }
            if (probe == NULL && regexec(&re_s, sp, 2, sm, 0) == 0) {
                size_t slen = (size_t)(sm[1].rm_eo - sm[1].rm_so);
                if (slen > sizeof(snippet) - 1)
                    slen = sizeof(snippet) - 1;
                __builtin_memcpy(snippet, sp + sm[1].rm_so, slen);
                snippet[slen] = '\0';
                builtin_strip_html(snippet);
            }
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

static int web_search_via_bing(const char *query, int max_results, char *buf, size_t buf_cap,
                               size_t *buf_len, int *count)
{
    char enc[2048];
    builtin_url_encode(query, enc, sizeof(enc));
    char cmd[8192];
    /* Primary search backend. Bing answers fast on most networks, so it is
     * tried first; the tight --max-time keeps the whole IPC round trip
     * inside the agent's 30s timeout even when the endpoint is unreachable. */
    snprintf(cmd, sizeof(cmd),
             "curl -sSL --max-time 20 -A \"Mozilla/5.0 (compatible; AirymaxRT/0.1.1 "
             "web_search)\" 'https://www.bing.com/search?q=%s'",
             enc);
    char *out = NULL;
    int exit_code = -1;

    if (builtin_shell_run(cmd, &out, &exit_code, 25000, NULL, NULL) != 0 || !out)
        return -1;
    if (exit_code != 0 || strstr(out, "command not found")) {
        AIRY_FREE(out);
        return -1;
    }
    int rc = web_search_extract(out, "<h2[^>]*><a[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a></h2>",
                                "class=\"b_lineclamp[^\"]*\"[^>]*>(.*?)</p>", max_results, buf,
                                buf_cap, buf_len, count);
    AIRY_FREE(out);
    return rc;
}

int web_search_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *q = cJSON_GetObjectItem(root, "query");
    if (!cJSON_IsString(q) || !q->valuestring || !q->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: query");
        return AIRY_ERR_INVALID_PARAM;
    }
    cJSON *mr = cJSON_GetObjectItem(root, "max_results");
    int max_results =
        (cJSON_IsNumber(mr) && mr->valueint > 0) ? mr->valueint : BUILTIN_WEBSEARCH_MAX;
    if (max_results > BUILTIN_WEBSEARCH_MAX)
        max_results = BUILTIN_WEBSEARCH_MAX;

    char *buf = (char *)AIRY_CALLOC(BUILTIN_OUTPUT_CAP, 1);
    if (!buf) {
        res->error = AIRY_STRDUP("Out of memory");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    size_t buf_len = 0;
    int count = 0;

    /* Primary path: Bing (fast on most networks), then DuckDuckGo as a
     * fallback for networks where Bing is blocked. Each endpoint is given a
     * tight --max-time so an unreachable search provider cannot stall the
     * IPC round trip beyond the agent's 30s timeout. */
    if (web_search_via_bing(q->valuestring, max_results, buf, BUILTIN_OUTPUT_CAP, &buf_len,
                            &count) != 0 ||
        count == 0) {
        char enc[2048];
        builtin_url_encode(q->valuestring, enc, sizeof(enc));
        char cmd[8192];
        snprintf(cmd, sizeof(cmd),
                 "curl -sSL --max-time 15 -A \"Mozilla/5.0 (compatible; "
                 "AirymaxRT/0.1.2 web_search)\" "
                 "'https://html.duckduckgo.com/html/?q=%s'",
                 enc);
        char *out = NULL;
        int exit_code = -1;

        int curl_ok = (builtin_shell_run(cmd, &out, &exit_code, 20000, NULL, NULL) == 0 && out &&
                       exit_code == 0 && !strstr(out, "command not found"));
        if (curl_ok) {
            web_search_extract(out, "class=\"result__a\" href=\"([^\"]+)\"[^>]*>([^<]+)</a>",
                               "class=\"result__snippet\"[^>]*>([^<]+)</a>", max_results, buf,
                               BUILTIN_OUTPUT_CAP, &buf_len, &count);
        }
        if (out)
            AIRY_FREE(out);
    }

    if (count == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "No web results for query: %s (Bing & DuckDuckGo unreachable)",
                 q->valuestring);
        res->error = AIRY_STRDUP(msg);
        AIRY_FREE(buf);
        res->success = 0;
        res->exit_code = 1;
        return AIRY_OK;
    }
    res->output = buf;
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}
#endif /* !_WIN32 */
