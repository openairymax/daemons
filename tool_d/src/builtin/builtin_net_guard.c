/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_net_guard.c
 * @brief SSRF host policy and search-provider configuration for the
 *        network built-in tools.
 *
 *        web_fetch must not become a pivot into the local host or the
 *        cloud metadata service: a model-supplied URL previously only had
 *        its characters filtered, so http://127.0.0.1/ or
 *        http://169.254.169.254/ were reachable. This unit denies loopback,
 *        private, link-local, metadata and reserved ranges by default (also
 *        after DNS resolution, so a public-looking name cannot resolve into
 *        a private address), and keeps an explicit operator allow list for
 *        legitimate intranet use.
 */

#include "builtin/builtin_net_internal.h"

#include "network_common_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 默认拒绝的 IPv4 网段；返回 1 表示命中。 */
static int ipv4_blocked(const unsigned char b[4])
{
    if (b[0] == 0) /* 0.0.0.0/8 "this network" */
        return 1;
    if (b[0] == 10) /* 10.0.0.0/8 */
        return 1;
    if (b[0] == 100 && (b[1] & 0xc0) == 64) /* 100.64.0.0/10 CGNAT */
        return 1;
    if (b[0] == 127) /* 127.0.0.0/8 loopback */
        return 1;
    if (b[0] == 169 && b[1] == 254) /* 169.254.0.0/16 link-local/metadata */
        return 1;
    if (b[0] == 172 && (b[1] & 0xf0) == 16) /* 172.16.0.0/12 */
        return 1;
    if (b[0] == 192 && b[1] == 168) /* 192.168.0.0/16 */
        return 1;
    if (b[0] == 198 && (b[1] == 18 || b[1] == 19)) /* 198.18.0.0/15 bench */
        return 1;
    if (b[0] >= 224) /* 224.0.0.0/4 multicast + 240.0.0.0/4 reserved */
        return 1;
    return 0;
}

/* 默认拒绝的 IPv6 网段；返回 1 表示命中。 */
static int ipv6_blocked(const unsigned char b[16])
{
    static const unsigned char v4mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    static const unsigned char zero[12] = {0};
    unsigned char v4[4];

    if (memcmp(b, zero, 12) == 0) { /* ::/96：未指定、::1 与 IPv4 兼容地址 */
        v4[0] = b[12];
        v4[1] = b[13];
        v4[2] = b[14];
        v4[3] = b[15];
        return (memcmp(b, zero, 16) == 0 || (v4[0] == 0 && v4[1] == 0 && v4[2] == 0 && v4[3] <= 1))
                   ? 1
                   : ipv4_blocked(v4);
    }
    if (memcmp(b, v4mapped, 12) == 0) { /* ::ffff:a.b.c.d */
        v4[0] = b[12];
        v4[1] = b[13];
        v4[2] = b[14];
        v4[3] = b[15];
        return ipv4_blocked(v4);
    }
    if ((b[0] & 0xfe) == 0xfc) /* fc00::/7 unique local */
        return 1;
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) /* fe80::/10 link-local */
        return 1;
    if (b[0] == 0xff) /* ff00::/8 multicast */
        return 1;
    return 0;
}

/* 严格点分十进制解析（4 段、0~255、禁止前导零）。
 * 非规范形态（0177、0x7f、2130706433）由 host_numeric 兜底拒绝。 */
static int ipv4_parse(const char *s, unsigned char b[4])
{
    int part = 0;
    while (part < 4) {
        const char *start = s;
        int val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if ((s - start) >= 3 || val > 255)
                return -1;
            s++;
        }
        if (s == start || ((s - start) > 1 && start[0] == '0'))
            return -1;
        b[part++] = (unsigned char)val;
        if (part == 4)
            break;
        if (*s != '.')
            return -1;
        s++;
    }
    return (*s == '\0') ? 0 : -1;
}

/* 是否“纯数字形态”主机（数字/点/十六进制前缀字符）。这类主机在
 * inet_aton/curl 中可被解释成 IP，但我们的严格解析不认，必须直接拒绝。 */
static int host_numeric(const char *h)
{
    int has_digit = 0;
    for (const char *p = h; *p; p++) {
        if (*p >= '0' && *p <= '9')
            has_digit = 1;
        else if (*p == '.' || *p == 'x' || *p == 'X' || (*p >= 'a' && *p <= 'f') ||
                 (*p >= 'A' && *p <= 'F'))
            continue;
        else
            return 0;
    }
    return has_digit;
}

/* 已知本地/内部域名后缀；返回 1 表示拒绝。 */
static int host_suffix_denied(const char *h)
{
    static const char *const suffixes[] = {".localhost", ".local", ".internal",
                                           ".lan", ".home.arpa", ".intranet"};
    if (strcmp(h, "localhost") == 0)
        return 1;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        size_t sl = strlen(suffixes[i]);
        size_t hl = strlen(h);
        if (hl > sl && strcmp(h + hl - sl, suffixes[i]) == 0)
            return 1;
    }
    return 0;
}

/* 大小写不敏感比较（不依赖 strncasecmp，Windows 上保持同一实现）。 */
static int token_eq(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + ('a' - 'A'));
        if (ca != cb)
            return 0;
    }
    return 1;
}

/* 运维放行列表匹配：逗号分隔、大小写不敏感、可带空白，"*" 全放行。 */
static int allow_match(const char *h)
{
    const char *env = getenv("AIRY_NET_ALLOW_HOSTS");
    if (!env || !env[0])
        return 0;
    while (*env) {
        while (*env == ',' || *env == ' ' || *env == '\t')
            env++;
        const char *start = env;
        while (*env && *env != ',')
            env++;
        const char *end = env;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        size_t len = (size_t)(end - start);
        if (len == 0)
            continue;
        if (len == 1 && start[0] == '*')
            return 1;
        if (len == strlen(h) && token_eq(start, h, len))
            return 1;
    }
    return 0;
}

/* 解析主机名并对每个返回地址套用默认拒绝规则。
 * 解析失败不拦截（连接层会给出真实错误），只拦“解析成功但落在禁区”。 */
static int resolve_denied(const char *host, char *why, size_t cap)
{
    struct addrinfo hints, *res = NULL;

    AIRY_MEMSET(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    network_init_winsock();
    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return 0;

    int denied = 0;
    for (struct addrinfo *p = res; p && !denied; p = p->ai_next) {
        char ip[46];
        if (p->ai_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)p->ai_addr;
            if (ipv4_blocked((const unsigned char *)&sa->sin_addr)) {
                inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                snprintf(why, cap, "host %s resolves to blocked address %s", host, ip);
                denied = 1;
            }
        } else if (p->ai_family == AF_INET6) {
            struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)p->ai_addr;
            if (ipv6_blocked((const unsigned char *)&sa6->sin6_addr)) {
                inet_ntop(AF_INET6, &sa6->sin6_addr, ip, sizeof(ip));
                snprintf(why, cap, "host %s resolves to blocked address %s", host, ip);
                denied = 1;
            }
        }
    }
    freeaddrinfo(res);
    return denied;
}

int net_ssrf_check(const char *host, char *why, size_t cap)
{
    char h[256];
    char sink[2];
    size_t hl;

    if (!why || cap == 0) { /* 允许调用方只关心判定结果 */
        why = sink;
        cap = sizeof(sink);
    }
    why[0] = '\0';
    if (!host || !host[0]) {
        snprintf(why, cap, "empty host");
        return -1;
    }
    hl = strlen(host);
    if (host[0] == '[' && hl >= 2 && host[hl - 1] == ']') { /* [::1] 形式 */
        hl -= 2;
        if (hl >= sizeof(h))
            hl = sizeof(h) - 1;
        AIRY_MEMCPY(h, host + 1, hl);
    } else {
        if (hl >= sizeof(h))
            hl = sizeof(h) - 1;
        AIRY_MEMCPY(h, host, hl);
    }
    h[hl] = '\0';
    for (size_t i = 0; i < hl; i++) { /* 大小写归一，放行表与后缀比较用 */
        if (h[i] >= 'A' && h[i] <= 'Z')
            h[i] = (char)(h[i] + ('a' - 'A'));
    }
    while (hl > 1 && h[hl - 1] == '.') /* FQDN 尾点 */
        h[--hl] = '\0';

    if (allow_match(h))
        return 0;

    if (strchr(h, ':') != NULL) {
        unsigned char b[16];
        if (inet_pton(AF_INET6, h, b) != 1) {
            snprintf(why, cap, "malformed IPv6 literal");
            return -1;
        }
        if (ipv6_blocked(b)) {
            snprintf(why, cap, "blocked IPv6 range (%s)", h);
            return -1;
        }
        return 0;
    }

    unsigned char b[4];
    if (ipv4_parse(h, b) == 0) {
        if (!ipv4_blocked(b))
            return 0;
        snprintf(why, cap, "blocked IPv4 range (%s)", h);
        return -1;
    }

    if (host_numeric(h)) {
        snprintf(why, cap,
                 "non-canonical numeric host %s (decimal/octal/hex IP forms are rejected)", h);
        return -1;
    }
    if (host_suffix_denied(h)) {
        snprintf(why, cap, "blocked local host name (%s)", h);
        return -1;
    }
    return resolve_denied(h, why, cap) ? -1 : 0;
}

int net_cfg_provider(char *ep, size_t ep_cap, char *key, size_t key_cap)
{
    const char *e = getenv("AIRY_WEB_SEARCH_ENDPOINT");
    const char *k = getenv("AIRY_WEB_SEARCH_KEY");

    if (ep && ep_cap)
        ep[0] = '\0';
    if (key && key_cap)
        key[0] = '\0';
    if (!e || !e[0])
        return 0;
    if (ep && ep_cap)
        snprintf(ep, ep_cap, "%s", e);
    if (key && key_cap && k)
        snprintf(key, key_cap, "%s", k);
    return 1;
}
