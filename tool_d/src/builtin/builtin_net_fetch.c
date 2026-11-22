/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_net_fetch.c
 * @brief Built-in tool web_fetch: fetch a page over the network.
 *
 *        Main path: curl child process (production-grade HTTPS/TLS +
 *        redirects + Content-Length parsing, sharing builtin_shell_run's
 *        timeout infrastructure with shell_run).
 *        Fallback path: when curl is unavailable, fall back to the
 *        network_common layer (http: only — the abstraction does not
 *        implement TLS yet, https needs curl).
 *
 *        Every URL passes the shared SSRF host policy before any request is
 *        issued (see builtin_net_guard.c).
 */

#include "builtin/builtin_net_internal.h"

#include "builtin/builtin_exec_internal.h"
#include "builtin/builtin_limits_internal.h"
#include "network_common.h"

/**
 * @brief Parse a URL into scheme/host/port/path (http/https only)
 * @return 0 on success, -1 on invalid URL
 */
/* 拆 scheme 并校验 http(s)；返回 host 起始位置，非法返回 NULL。 */
static const char *url_parse_scheme(const char *url, builtin_url_t *u)
{
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end || scheme_end == url)
        return NULL;
    size_t slen = (size_t)(scheme_end - url);
    if (slen >= sizeof(u->scheme))
        return NULL;
    AIRY_MEMCPY(u->scheme, url, slen);
    u->scheme[slen] = '\0';
    if (strcmp(u->scheme, "http") != 0 && strcmp(u->scheme, "https") != 0)
        return NULL;
    return scheme_end + 3;
}

/* 解析 host；返回端口/路径分界，非法返回 NULL。 */
static const char *url_parse_host(const char *host_start, builtin_url_t *u)
{
    const char *p = host_start;
    while (*p && *p != ':' && *p != '/' && *p != '?')
        p++;
    size_t hlen = (size_t)(p - host_start);
    if (hlen == 0 || hlen >= sizeof(u->host))
        return NULL;
    AIRY_MEMCPY(u->host, host_start, hlen);
    u->host[hlen] = '\0';
    return p;
}

/* 解析端口段（可缺省）；返回路径起点，非法返回 NULL。 */
static const char *url_parse_port(const char *p, builtin_url_t *u)
{
    if (*p != ':')
        return p;
    const char *port_start = p + 1;
    const char *q = port_start;
    while (*q && *q != '/' && *q != '?') {
        if (*q < '0' || *q > '9')
            return NULL;
        q++;
    }
    if (q == port_start)
        return NULL;
    long port = strtol(port_start, NULL, 10);
    if (port <= 0 || port > 65535)
        return NULL;
    u->port = (int)port;
    return q;
}

/* 解析 path（缺省 "/"）；返回 0/-1。 */
static int url_parse_path(const char *path_start, builtin_url_t *u)
{
    if (*path_start == '\0') {
        u->path[0] = '/';
        u->path[1] = '\0';
        return 0;
    }
    if (strlen(path_start) >= sizeof(u->path))
        return -1;
    AIRY_MEMCPY(u->path, path_start, strlen(path_start) + 1);
    return 0;
}

int net_url_parse(const char *url, builtin_url_t *u)
{
    const char *host_start = url_parse_scheme(url, u);
    if (!host_start)
        return -1;
    const char *path_start = url_parse_host(host_start, u);
    if (!path_start)
        return -1;
    u->port = (strcmp(u->scheme, "https") == 0) ? 443 : 80;
    path_start = url_parse_port(path_start, u);
    if (!path_start)
        return -1;
    return url_parse_path(path_start, u);
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
    res->err_code = (resp.status_code >= 200 && resp.status_code < 400) ? AIRY_OK : AIRY_ERR_IO;
    res->exit_code = resp.status_code;
    if (res->err_code != AIRY_OK)
        btool_fail(res, AIRY_ERR_IO, "HTTP error status %d", resp.status_code);
    network_http_response_free(&resp);
    return AIRY_OK;
}
#endif

/* 校验 URL 形态、解析出 scheme/host/port/path，并套用 SSRF 主机策略；
 * 合法返回 0。 */
static int fetch_check_url(const char *url_str, builtin_url_t *u, tool_result_t *res)
{
    char why[160];

    if (strpbrk(url_str, "'\"`$\\") != NULL)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "URL contains unsafe characters (', \", `, $, \\)");
    if (net_url_parse(url_str, u) != 0)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "Invalid URL (only http:// and https:// are supported)");
    if (net_ssrf_check(u->host, why, sizeof(why)) != 0)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "URL host rejected: %s", why);
    return 0;
}

/* 剥离 curl -w 追加的状态标记并取回 HTTP 状态码。 */
static void fetch_pop_status(char *out, long *http_status)
{
    char *mark = out ? strstr(out, "\n__AIRY_STATUS__:") : NULL;
    if (!mark)
        return;
    *mark = '\0';
    *http_status = strtol(mark + strlen("\n__AIRY_STATUS__:"), NULL, 10);
}

/* 按状态码落结果：4xx/5xx → IO 错误；>0 → OK；缺标记 → curl 整体失败。 */
static void fetch_apply_status(tool_result_t *res, long http_status, int exit_code)
{
    if (http_status >= 400) {
        btool_fail(res, AIRY_ERR_IO, "HTTP error status %ld", http_status);
        res->exit_code = (int)http_status;
        return;
    }
    if (http_status > 0) {
        res->err_code = AIRY_OK;
        res->exit_code = 0;
        return;
    }
    btool_fail(res, AIRY_ERR_IO, "Web fetch failed (curl exit %d): %s", exit_code,
               res->output ? res->output : "");
    AIRY_FREE(res->output);
    res->output = AIRY_STRDUP("");
    res->exit_code = exit_code;
}

int web_fetch_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });
    cJSON *url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(url) || !url->valuestring || !url->valuestring[0])
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "Missing string parameter: url");
    const char *url_str = url->valuestring;

    builtin_url_t u;
    int uerr = fetch_check_url(url_str, &u, res);
    if (uerr != 0)
        return uerr;

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
                               NULL, 0);
    if (rc != 0)
        return btool_fail(res, AIRY_ERR_EXEC_FAIL,
                          "Failed to execute web fetch (pipe/process creation failed)");

    if (exit_code == 127 || (out && strstr(out, "command not found"))) {
        AIRY_FREE(out);
#ifndef _WIN32
        /* Fallback: plain-HTTP via the built-in network layer (no TLS). */
        if (strcmp(u.scheme, "http") == 0) return web_fetch_via_network(&u, res);
#endif
        return btool_fail(res, AIRY_ERR_NOT_SUPPORTED,
                          "curl is not available and %s:// requires curl (TLS not supported by "
                          "the built-in network layer)",
                          u.scheme);
    }

    long http_status = 0;
    fetch_pop_status(out, &http_status);
    res->output = out ? out : AIRY_STRDUP("");

    fetch_apply_status(res, http_status, exit_code);
    return AIRY_OK;
}
