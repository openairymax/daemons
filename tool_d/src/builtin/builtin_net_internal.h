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
 *
 *        Serves as the family prelude: the result-envelope helpers
 *        (btool_fail/btool_soft) live in builtin_result_internal.h and the
 *        plain runner contract in builtin_shell_internal.h, so the net
 *        units include the two narrow headers here instead of each
 *        reaching for the limits/exec internals (G13 fan-in ratchet).
 */

#ifndef AIRY_RT_TOOL_BUILTIN_NET_INTERNAL_H
#define AIRY_RT_TOOL_BUILTIN_NET_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "builtin/builtin_result_internal.h"
#include "builtin/builtin_shell_internal.h"

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
 * 默认拒绝环回/私网/链路本地/元数据/保留网段，含 IPv4 映射 IPv6、
 * 十进制/八进制/十六进制等非规范数字形态，以及 host 里混入 @ # % 等
 * 会被 curl 解释成 userinfo/片段/转义的字符。环境变量
 * AIRY_NET_ALLOW_HOSTS（逗号分隔主机名或 IP，"*" 全放行）是显式运维
 * 放行列表，命中即跳过全部默认拒绝规则。 */
int net_ssrf_check(const char *host, char *why, size_t cap);

/* 校验任意 http(s) URL：字符集、scheme/host/port/path 解析与 SSRF 主机
 * 策略。0 允许，-1 拒绝（why 写原因）。重定向目标必须逐跳走这里。 */
int net_url_guard(const char *url, char *why, size_t cap);

/* 跟随重定向的 GET 请求描述。extra_argv 是追加给 curl 的参数数组
 * （NULL 结尾，不含程序名），以 argv 形式传递、不经 shell。 */
typedef struct {
    const char *url;
    char *const *extra_argv;
    uint32_t timeout_ms; /* 整条重定向链的总预算；0 = 无预算 */
} net_http_req_t;

typedef struct {
    char *body;    /* AIRY_MALLOC 分配，调用方 AIRY_FREE */
    long status;   /* 最后一跳 HTTP 状态码；0 = 无 */
    int exit_code; /* 最后一跳 curl 退出码 */
    int redirects; /* 实际跟随的跳数 */
    char why[160];
} net_http_resp_t;

/* 执行 GET 并跟随重定向，每一跳都复检 net_url_guard。
 * 返回 0 = 已得到最终响应（含 4xx/5xx 与 curl 失败）；
 *      1 = curl 不可用（调用方可回退）；
 *     -1 = 被策略拒绝 / 超跳 / 超预算 / 无法建临时文件。 */
int net_http_get(const net_http_req_t *req, net_http_resp_t *resp);

/* 从 curl -w 追加的元数据尾解析状态码与重定向目标，并把正文截断到
 * 元数据之前。标记由调用方生成（含前导换行），取最后一次出现。
 * 返回 0 成功，-1 未找到合法尾部。 */
int net_http_meta_parse(char *out, const char *status_mark, const char *redir_mark, long *status,
                        char *redir, size_t redir_cap);

/* 读取可配置搜索 provider：endpoint 为含 %s 的 JSON API URL 模板
 * （无 %s 时按 ?q=/%s 追加），key 可为空。返回 1=已配置 endpoint，
 * 0=未配置。缓冲区内容在返回 0 时为空串。 */
int net_cfg_provider(char *ep, size_t ep_cap, char *key, size_t key_cap);

/* 解析 provider 的 JSON 响应，按 "[N] title\n    url\n    snippet\n"
 * 写入 buf 并累计长度；返回提取到的条目数（0 表示无结果或非法 JSON）。 */
int net_search_json(const char *json, int max, char *buf, size_t cap, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_BUILTIN_NET_INTERNAL_H */
