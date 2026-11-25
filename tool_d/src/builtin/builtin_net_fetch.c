/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_net_fetch.c
 * @brief Built-in tool web_fetch: fetch a page over the network.
 *
 *        Single path: in-process libcurl transport (HTTPS/TLS + per-hop
 *        redirect re-check + Content-Length parsing) via builtin_net_http.c,
 *        sharing the per-request deadline with the rest of the builtin tools.
 *
 *        Every URL passes the shared SSRF host policy before any request is
 *        issued (see builtin_net_guard.c).
 */

#include "builtin/builtin_net_internal.h"

#include "airy_prov/provider_transport.h"

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

/* 校验 URL 形态、解析出 scheme/host/port/path，并套用统一 URL 策略
 * （net_url_guard：字符集 + scheme + SSRF 主机）；合法返回 0。 */
static int fetch_check_url(const char *url_str, builtin_url_t *u, tool_result_t *res)
{
    char why[160];

    if (net_url_guard(url_str, why, sizeof(why)) != 0)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "URL rejected: %s", why);
    if (net_url_parse(url_str, u) != 0)
        return btool_fail(res, AIRY_ERR_INVALID_PARAM,
                          "Invalid URL (only http:// and https:// are supported)");
    return 0;
}

/* 按状态码落结果：4xx/5xx 或 0 状态码都判失败，其余成功。 */
static void fetch_apply_status(tool_result_t *res, long http_status)
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
    btool_fail(res, AIRY_ERR_IO, "Web fetch failed: no HTTP status");
    AIRY_FREE(res->output);
    res->output = AIRY_STRDUP("");
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

    /* Main path: in-process libcurl, no redirect following in the transfer.
     * net_http_get 逐跳跟随重定向并在每一跳复检 SSRF 策略——盲跟会让
     * 已通过首跳校验的 URL 被 302 到元数据/环回地址。
     *
     * 超时预算（2026-08-19）：CLI 侧工具 RPC 超时为 30s（cli_chat.c
     * CLI_TOOL_RPC_TIMEOUT_MS），整链预算若同样 30s，慢网页会先撞 RPC
     * 超时（实测 rc=-31 超时）。整链预算 25s 给 RPC 返回留出余量。 */
    struct curl_slist *hdr = curl_slist_append(NULL, "User-Agent: AirymaxRT/0.1.2 web_fetch");
    net_http_req_t req;
    req.url = url_str;
    req.headers = hdr;
    req.timeout_ms = timeout_ms ? timeout_ms : 25000;

    net_http_resp_t rsp = {0};
    int rc = net_http_get(&req, &rsp);
    if (hdr)
        curl_slist_free_all(hdr);
    if (rc != 0)
        return btool_fail(res, AIRY_ERR_IO, "Web fetch rejected: %s", rsp.why);

    res->output = rsp.body ? rsp.body : AIRY_STRDUP("");

    fetch_apply_status(res, rsp.status);
    return AIRY_OK;
}
