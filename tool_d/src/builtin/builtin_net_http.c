/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file builtin_net_http.c
 * @brief Redirect-safe HTTP(S) GET shared by web_fetch and web_search.
 *
 *        Two defects are closed here:
 *
 *        1. curl -L followed an arbitrary redirect chain without telling the
 *           caller which host it was about to contact, so a model-supplied URL
 *           could pass the initial SSRF check and then be redirected into the
 *           cloud metadata service (169.254.169.254) or loopback. This unit
 *           never passes -L: every hop is issued separately without following,
 *           the status and Location are read from a nonce-tagged --write-out
 *           trailer, and the full URL policy is re-run on each redirect target.
 *
 *        2. The request used to be assembled into a "curl ... '%s'" shell
 *           string. cmd.exe treats single quotes literally and expands %VAR%,
 *           so on Windows the URL and trailer were mangled (percent-encoded
 *           UTF-8 queries in particular), and any quote in a URL was command
 *           injection. curl is now spawned through airy_process_run_capture
 *           with an argv array: no shell, identical semantics on every
 *           platform.
 *
 *        The body is written to a private temp file (curl -o) so the trailer
 *        on stdout cannot be truncated by a large page and page content cannot
 *        forge a trailer.
 */

#include "builtin/builtin_net_internal.h"

#include "builtin/builtin_limits_internal.h"
#include "builtin/builtin_time_internal.h"
#include "platform_process.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define NET_HTTP_MAX_HOPS 5U
#define NET_HTTP_MIN_MS 500U
#define NET_HTTP_DEFAULT_HOP_MS 20000U
#define NET_HTTP_MAX_ARGS 24

/* 取最后一次出现的标记：正文里伪造的标记会被真正的尾部覆盖。 */
static char *last_mark(char *out, const char *mark)
{
    char *last = NULL;
    char *p = out;
    size_t len = strlen(mark);

    while ((p = strstr(p, mark)) != NULL) {
        last = p;
        p += len;
    }
    return last;
}

int net_http_meta_parse(char *out, const char *status_mark, const char *redir_mark, long *status,
                        char *redir, size_t redir_cap)
{
    if (!out || !status_mark || !redir_mark || !status)
        return -1;

    char *m = last_mark(out, status_mark);
    if (!m)
        return -1;
    char *p = m + strlen(status_mark);
    if (*p < '0' || *p > '9')
        return -1;

    long code = strtol(p, &p, 10);
    if (strncmp(p, redir_mark, strlen(redir_mark)) != 0)
        return -1;
    p += strlen(redir_mark);

    char *nl = strchr(p, '\n');
    if (nl)
        *nl = '\0';
    if (redir && redir_cap) {
        size_t len = strlen(p);
        if (len >= redir_cap)
            len = redir_cap - 1;
        AIRY_MEMCPY(redir, p, len);
        redir[len] = '\0';
    }
    *m = '\0';
    *status = code;
    return 0;
}

/* 本跳预算：整链预算已耗尽返回 0，无预算返回默认值。 */
static uint32_t hop_budget(uint64_t deadline)
{
    int left = builtin_deadline_left_ms(deadline);

    if (left == 0)
        return 0;
    if (left < 0)
        return NET_HTTP_DEFAULT_HOP_MS;
    return (uint32_t)left;
}

/* 私有临时体文件：POSIX 用 mkstemp，Windows 用 GetTempFileNameA。 */
static int temp_body_open(char *path, size_t cap)
{
#ifdef _WIN32
    char dir[MAX_PATH + 1];
    DWORD n = GetTempPathA((DWORD)sizeof(dir), dir);
    if (n == 0 || n >= sizeof(dir))
        return -1;
    if (GetTempFileNameA(dir, "air", 0, path) == 0)
        return -1;
    if (strlen(path) >= cap) {
        DeleteFileA(path);
        return -1;
    }
    return 0;
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0])
        dir = "/tmp";
    int w = snprintf(path, cap, "%s/airy_net_XXXXXX", dir);
    if (w <= 0 || (size_t)w >= cap)
        return -1;
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
#endif
}

static void temp_body_remove(const char *path)
{
    if (!path || !path[0])
        return;
#ifdef _WIN32
    DeleteFileA(path);
#else
    unlink(path);
#endif
}

/* 读取体积上限与 shell 捕获上限一致（1MB），保持既有截断语义。 */
static char *temp_body_read(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = (char *)AIRY_MALLOC(BUILTIN_OUTPUT_CAP);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, BUILTIN_OUTPUT_CAP - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* 组装 argv：curl -sS --max-time N -o <body> -w <meta> [extra...] <url>。 */
static int build_argv(const net_http_req_t *req, const char *url, const char *body_path,
                      const char *meta, uint32_t budget_ms, char *max_time, size_t max_time_cap,
                      char **argv, size_t argv_cap)
{
    uint32_t secs = (budget_ms + 999U) / 1000U;
    size_t n = 0;

    if (secs == 0)
        secs = 1;
    snprintf(max_time, max_time_cap, "%u", (unsigned)secs);

    argv[n++] = (char *)"curl";
    argv[n++] = (char *)"-sS";
    argv[n++] = (char *)"--max-time";
    argv[n++] = max_time;
    argv[n++] = (char *)"-o";
    argv[n++] = (char *)body_path;
    argv[n++] = (char *)"-w";
    argv[n++] = (char *)meta;
    if (req->extra_argv) {
        for (char *const *p = req->extra_argv; *p; p++) {
            if (n + 2 >= argv_cap)
                return -1;
            argv[n++] = *p;
        }
    }
    if (n + 2 > argv_cap)
        return -1;
    argv[n++] = (char *)url;
    argv[n] = NULL;
    return 0;
}

int net_http_get(const net_http_req_t *req, net_http_resp_t *resp)
{
    char cur[4096];
    char body_path[512];
    char status_mark[64];
    char redir_mark[64];
    char meta[256];
    char max_time[16];
    char *argv[NET_HTTP_MAX_ARGS];

    if (!req || !resp || !req->url || !req->url[0])
        return -1;
    AIRY_MEMSET(resp, 0, sizeof(*resp));
    if (strlen(req->url) >= sizeof(cur)) {
        snprintf(resp->why, sizeof(resp->why), "request URL too long");
        return -1;
    }
    AIRY_MEMCPY(cur, req->url, strlen(req->url) + 1);
    if (temp_body_open(body_path, sizeof(body_path)) != 0) {
        snprintf(resp->why, sizeof(resp->why), "cannot create temporary response file");
        return -1;
    }

    /* 标记必须不可预测（否则跳转目标可在 Location 里伪造尾部），又不能
     * 依赖静态计数器：tool_d 是同进程多线程的，++seq 是数据竞争。用本次
     * 调用的 resp 地址与单调时钟混合，天然每请求不同且无共享状态。 */
    unsigned nonce = (unsigned)(builtin_deadline_ms(1) & 0xffffffffu) ^ (unsigned)(uintptr_t)resp;
    snprintf(status_mark, sizeof(status_mark), "\n__AIRY_%08x_STATUS__:", nonce);
    snprintf(redir_mark, sizeof(redir_mark), "\n__AIRY_%08x_REDIR__:", nonce);
    snprintf(meta, sizeof(meta),
             "\\n__AIRY_%08x_STATUS__:%%{http_code}\\n__AIRY_%08x_REDIR__:%%{redirect_url}", nonce,
             nonce);

    uint64_t deadline = builtin_deadline_ms(req->timeout_ms);
    char *out = (char *)AIRY_MALLOC(BUILTIN_OUTPUT_CAP);
    int hops = 0;
    int ret = -1;

    if (!out) {
        snprintf(resp->why, sizeof(resp->why), "out of memory");
        temp_body_remove(body_path);
        return -1;
    }

    for (;;) {
        uint32_t budget = hop_budget(deadline);
        if (budget < NET_HTTP_MIN_MS) {
            snprintf(resp->why, sizeof(resp->why), "time budget exhausted before the request");
            break;
        }
        if (build_argv(req, cur, body_path, meta, budget, max_time, sizeof(max_time), argv,
                       NET_HTTP_MAX_ARGS) != 0) {
            snprintf(resp->why, sizeof(resp->why), "too many curl arguments");
            break;
        }

        out[0] = '\0';
        int exit_code = airy_process_run_capture("curl", argv, NULL, budget, out,
                                                 BUILTIN_OUTPUT_CAP);

        /* POSIX 侧 exec 失败由子进程以 127 退出；Windows 侧启动失败返回
         * AIRY_ERR_EXEC_FAIL(-502)。curl 自身不使用 127，故两者都表示
         * curl 不可用。-2 = 超时、-3 = 取消，不属于启动失败。 */
        if (exit_code == 127 ||
            (exit_code < 0 && exit_code != -2 && exit_code != -3)) {
            snprintf(resp->why, sizeof(resp->why), "curl executable not found");
            ret = 1;
            break;
        }

        long status = 0;
        char redir[4096];
        redir[0] = '\0';
        if (net_http_meta_parse(out, status_mark, redir_mark, &status, redir, sizeof(redir)) !=
            0) {
            resp->body = AIRY_STRDUP("");
            resp->status = 0;
            resp->exit_code = exit_code;
            resp->redirects = hops;
            snprintf(resp->why, sizeof(resp->why), "curl produced no status trailer (exit %d)",
                     exit_code);
            ret = 0;
            break;
        }

        if (status >= 300 && status < 400 && redir[0]) {
            if (hops >= (int)NET_HTTP_MAX_HOPS) {
                snprintf(resp->why, sizeof(resp->why),
                         "too many redirects (more than %u)", (unsigned)NET_HTTP_MAX_HOPS);
                resp->status = status;
                resp->exit_code = exit_code;
                break;
            }
            char why[160];
            if (net_url_guard(redir, why, sizeof(why)) != 0) {
                snprintf(resp->why, sizeof(resp->why), "redirect rejected: %s", why);
                resp->status = status;
                resp->exit_code = exit_code;
                break;
            }
            if (strlen(redir) >= sizeof(cur)) {
                snprintf(resp->why, sizeof(resp->why), "redirect URL too long");
                resp->status = status;
                resp->exit_code = exit_code;
                break;
            }
            AIRY_MEMCPY(cur, redir, strlen(redir) + 1);
            hops++;
            continue;
        }

        resp->body = temp_body_read(body_path);
        if (!resp->body)
            resp->body = AIRY_STRDUP("");
        resp->status = status;
        resp->exit_code = exit_code;
        resp->redirects = hops;
        ret = 0;
        break;
    }

    AIRY_FREE(out);
    temp_body_remove(body_path);
    return ret;
}
