/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_net_internal.h
 * @brief Network built-in tools internal contract: URL shape, SSRF host
 *        policy, configurable search provider and provider JSON parsing.
 *
 *        The network domain is split into three translation units so each
 *        one stays small and independently testable:
 *          - builtin_net_fetch.c  web_fetch (URL parse + curl + fallback)
 *          - builtin_net_search.c web_search (provider JSON + SERP scrape)
 *          - builtin_net_guard.c  SSRF host policy + provider config reader
 */

#ifndef AIRY_RT_TOOL_BUILTIN_NET_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_NET_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char scheme[8]; /* "http" / "https" */
    char host[256];
    int port;
    char path[2048];
} builtin_url_t;

/* 解析 http(s) URL 为 scheme/host/port/path；0 成功，-1 非法。 */
int net_url_parse(const char *url, builtin_url_t *u);

/* SSRF 主机策略：0 允许，-1 拒绝（why 写入拒绝原因）。
 * 默认拒绝环回/私网/链路本地/元数据/保留网段，含 IPv4 映射 IPv6 与
 * 十进制/八进制/十六进制等非规范数字形态。环境变量
 * AIRY_NET_ALLOW_HOSTS（逗号分隔主机名或 IP，"*" 全放行）是显式运维
 * 放行列表，命中即跳过全部默认拒绝规则。 */
int net_ssrf_check(const char *host, char *why, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_NET_INTERNAL_H */
