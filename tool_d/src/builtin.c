// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
//
// @file builtin.c
// @brief tool_d 内置基础工具集（真实实现，非桩）：
//   fs_read / fs_write / fs_list / shell_run / web_fetch
//
// 设计说明：
// - 内置工具是 Agent 基础能力的开箱即用集（对标 Claude Code / OpenAI Codex
//   的 fs 与 shell 工具），通过 daemon_security ACL 显式授权后可用。
// - 所有工具接收 params_json（OpenAI tool_call arguments），返回结果写入
//   tool_result_t（output=stdout 语义 / error=stderr 语义 / exit_code）。
// - shell_run 使用 popen 真实执行命令（agent 端命令执行能力），权限由
//   上层 approval（fail-closed ACL）管控。
//
// 安全边界：
// - fs 操作与 shell 执行均为真实 I/O，由 approval 层（daemon_security ACL
//   fail-closed）决定是否放行；未授权工具一律拒绝。

#include "airy_memory.h"
#include "error.h"

#include "builtin.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <cjson_helpers.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* C-NET: 网络抽象层（HTTP 客户端，web_fetch 联网工具） */
#include "network_common.h"

#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

/* 单次输出上限（防失控输出） */
#define BUILTIN_OUTPUT_CAP (1U << 20) /* 1MB */
/* shell_run 超时（与 tool_d 元数据 timeout_sec=60 对齐，防永久阻塞） */
#define BUILTIN_SHELL_TIMEOUT_MS 60000

/* 读取全部 stdin（popen/fopen 管道），返回堆分配字符串 */
static char *builtin_read_all(FILE *fp, int *out_truncated)
{
    if (out_truncated)
        *out_truncated = 0;
    if (!fp)
        return NULL;
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf)
        return NULL;
    char chunk[4096];
    for (;;) {
        size_t n = fread(chunk, 1, sizeof(chunk), fp);
        if (n == 0)
            break;
        if (len + n + 1 > cap) {
            size_t new_cap = cap * 2;
            if (new_cap > BUILTIN_OUTPUT_CAP)
                new_cap = BUILTIN_OUTPUT_CAP;
            if (new_cap <= cap) {
                /* 达到上限：截断 */
                len = cap - 1;
                break;
            }
            char *nb = (char *)AIRY_REALLOC(buf, new_cap);
            if (!nb)
                break;
            buf = nb;
            cap = new_cap;
        }
        if (len + n >= cap) {
            n = cap - len - 1;
            __builtin_memcpy(buf + len, chunk, n);
            len += n;
            break;
        }
        __builtin_memcpy(buf + len, chunk, n);
        len += n;
    }
    buf[len] = '\0';
    if (out_truncated) {
        *out_truncated = (len >= cap - 1);
    }
    return buf;
}

#ifndef _WIN32
/* 追加截断标记到缓冲区末尾（调用者保证 cap > len + 1） */
static void builtin_append_trunc_mark(char *buf, size_t cap, size_t len, const char *mark)
{
    size_t mlen = strlen(mark);
    if (len + mlen + 1 <= cap) {
        __builtin_memcpy(buf + len, mark, mlen);
        buf[len + mlen] = '\0';
    } else {
        buf[len] = '\0';
    }
}

/**
 * @brief 带超时执行 shell 命令并捕获 stdout/stderr（fork + pipe + poll + waitpid）
 *
 * 替代 popen：命令超过 timeout_ms 即 SIGKILL 子进程，杜绝 tool_d 永久阻塞。
 * @param cmd 命令字符串（由 /bin/sh -c 解释）
 * @param out 捕获输出（AIRY_MALLOC，调用者释放）
 * @param exit_code 进程退出码（超时置 -1）
 * @param timeout_ms 超时毫秒
 * @param out_truncated 输出是否被截断
 * @return 0 成功，非 0 失败（fork/pipe/OOM）
 */
static int builtin_shell_run(const char *cmd, char **out, int *exit_code,
                             uint32_t timeout_ms, int *out_truncated)
{
    *out = NULL;
    *exit_code = -1;
    if (out_truncated)
        *out_truncated = 0;

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* 子进程：stdout/stderr 合并重定向到管道 */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);

    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipefd[0]);
        return -1;
    }
    buf[0] = '\0';

    int wstatus = 0;
    int exited = 0;
    int timed_out = 0;
    int truncated = 0;

    /* 单调时钟截止时间：快速写入的子进程不会饿死超时检查 */
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t deadline_ms = (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000 +
                           timeout_ms;

    for (;;) {
        if (!exited) {
            pid_t w = waitpid(pid, &wstatus, WNOHANG);
            if (w == pid)
                exited = 1;
        }
        if (exited)
            break;
        /* 超时检查：每次循环都依据单调时钟判定，与数据流量无关 */
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        uint64_t now_ms = (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;
        if (now_ms >= deadline_ms) {
            timed_out = 1;
            break;
        }
        /* poll 100ms：读取子进程已写入的数据（不阻塞于空管道） */
        struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN};
        int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            char chunk[4096];
            ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
            if (n > 0) {
                if (len + (size_t)n + 1 > cap) {
                    size_t new_cap = cap * 2;
                    if (new_cap > BUILTIN_OUTPUT_CAP)
                        new_cap = BUILTIN_OUTPUT_CAP;
                    if (new_cap <= cap) {
                        truncated = 1;
                        break;
                    }
                    char *nb = (char *)AIRY_REALLOC(buf, new_cap);
                    if (!nb) {
                        truncated = 1;
                        break;
                    }
                    buf = nb;
                    cap = new_cap;
                }
                if (len + (size_t)n >= cap) {
                    n = (ssize_t)(cap - len - 1);
                    truncated = 1;
                }
                __builtin_memcpy(buf + len, chunk, (size_t)n);
                len += (size_t)n;
            }
        }
    }

    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }

    /* 排空剩余输出（子进程已退出/被杀，read 不会阻塞） */
    for (;;) {
        char chunk[4096];
        ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
        if (n <= 0)
            break;
        if (len + (size_t)n + 1 > cap) {
            size_t new_cap = cap * 2;
            if (new_cap > BUILTIN_OUTPUT_CAP)
                new_cap = BUILTIN_OUTPUT_CAP;
            if (new_cap <= cap) {
                truncated = 1;
                break;
            }
            char *nb = (char *)AIRY_REALLOC(buf, new_cap);
            if (!nb) {
                truncated = 1;
                break;
            }
            buf = nb;
            cap = new_cap;
        }
        if (len + (size_t)n >= cap) {
            n = (ssize_t)(cap - len - 1);
            truncated = 1;
        }
        __builtin_memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    close(pipefd[0]);

    if (timed_out) {
        const char mark[] = "\n[command timed out after 60s]";
        builtin_append_trunc_mark(buf, cap, len, mark);
        len += sizeof(mark) - 1;
        if (len >= cap)
            len = cap - 1;
        buf[len] = '\0';
        *exit_code = -1;
    } else if (exited) {
#ifdef WIFEXITED
        *exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
#else
        *exit_code = wstatus;
#endif
    } else {
        *exit_code = -1;
    }
    if (truncated) {
        const char mark[] = "\n[output truncated at 1MB]";
        builtin_append_trunc_mark(buf, cap, len, mark);
        len += sizeof(mark) - 1;
        if (len >= cap)
            len = cap - 1;
        buf[len] = '\0';
    }
    if (out_truncated)
        *out_truncated = truncated;
    *out = buf;
    return 0;
}
#endif

static int fs_read_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(path) || !path->valuestring || !path->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: path");
        return AIRY_ERR_INVALID_PARAM;
    }
#ifndef _WIN32
    FILE *fp = fopen(path->valuestring, "rb");
    if (!fp) {
        char err[512];
        snprintf(err, sizeof(err), "Cannot open file '%s': %s", path->valuestring,
                 strerror(errno));
        res->error = AIRY_STRDUP(err);
        return (errno == ENOENT) ? AIRY_ERR_NOT_FOUND : AIRY_ERR_IO;
    }
    int truncated = 0;
    char *content = builtin_read_all(fp, &truncated);
    fclose(fp);
    if (!content) {
        res->error = AIRY_STRDUP("Failed to read file (I/O error)");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    if (truncated) {
        /* 超限输出：用截断标记覆盖缓冲区尾部，避免静默丢数据 */
        const char mark[] = "[output truncated at 1MB]";
        __builtin_memcpy(content + BUILTIN_OUTPUT_CAP - sizeof(mark), mark, sizeof(mark));
    }
    res->output = content;
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
#else
    res->error = AIRY_STRDUP("fs_read is not supported on this platform");
    return AIRY_ERR_NOT_SUPPORTED;
#endif
}

static int fs_write_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (!cJSON_IsString(path) || !path->valuestring || !path->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: path");
        return AIRY_ERR_INVALID_PARAM;
    }
    if (!cJSON_IsString(content) || !content->valuestring) {
        res->error = AIRY_STRDUP("Missing string parameter: content");
        return AIRY_ERR_INVALID_PARAM;
    }
#ifndef _WIN32
    FILE *fp = fopen(path->valuestring, "wb");
    if (!fp) {
        char err[512];
        snprintf(err, sizeof(err), "Cannot write file '%s': %s", path->valuestring,
                 strerror(errno));
        res->error = AIRY_STRDUP(err);
        return AIRY_ERR_IO;
    }
    size_t clen = strlen(content->valuestring);
    size_t written = fwrite(content->valuestring, 1, clen, fp);
    fclose(fp);
    if (written != clen) {
        char err[256];
        snprintf(err, sizeof(err), "Short write to '%s' (%zu/%zu bytes)", path->valuestring,
                 written, clen);
        res->error = AIRY_STRDUP(err);
        return AIRY_ERR_IO;
    }
    char ok[512];
    snprintf(ok, sizeof(ok), "Written %zu bytes to %s", written, path->valuestring);
    res->output = AIRY_STRDUP(ok);
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
#else
    res->error = AIRY_STRDUP("fs_write is not supported on this platform");
    return AIRY_ERR_NOT_SUPPORTED;
#endif
}

static int fs_list_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    const char *dir = NULL;
    if (cJSON_IsString(path) && path->valuestring && path->valuestring[0]) {
        dir = path->valuestring;
    }
#ifndef _WIN32
    DIR *d = opendir(dir ? dir : ".");
    if (!d) {
        char err[512];
        snprintf(err, sizeof(err), "Cannot open directory '%s': %s", dir ? dir : ".",
                 strerror(errno));
        res->error = AIRY_STRDUP(err);
        return (errno == ENOENT) ? AIRY_ERR_NOT_FOUND : AIRY_ERR_IO;
    }
    cJSON *arr = cJSON_CreateArray();
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", ent->d_name);
#ifdef DT_DIR
        cJSON_AddStringToObject(item, "type", ent->d_type == DT_DIR ? "dir" : "file");
#endif
        cJSON_AddItemToArray(arr, item);
    }
    closedir(d);
    res->output = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!res->output) {
        res->error = AIRY_STRDUP("Failed to serialize directory listing");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
#else
    res->error = AIRY_STRDUP("fs_list is not supported on this platform");
    return AIRY_ERR_NOT_SUPPORTED;
#endif
}

static int shell_run_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(cmd) || !cmd->valuestring || !cmd->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: command");
        return AIRY_ERR_INVALID_PARAM;
    }
#ifndef _WIN32
    char *out = NULL;
    int exit_code = -1;
    int rc = builtin_shell_run(cmd->valuestring, &out, &exit_code, BUILTIN_SHELL_TIMEOUT_MS,
                               NULL);
    if (rc != 0) {
        res->error = AIRY_STRDUP("Failed to execute command (fork/pipe failed)");
        return AIRY_ERR_EXEC_FAIL;
    }
    res->output = out ? out : AIRY_STRDUP("");
    res->success = (exit_code == 0) ? 1 : 0;
    res->exit_code = exit_code;
    if (exit_code != 0) {
        char err[256];
        snprintf(err, sizeof(err), "Command exited with code %d", exit_code);
        res->error = AIRY_STRDUP(err);
    }
    return AIRY_OK;
#else
    res->error = AIRY_STRDUP("shell_run is not supported on this platform");
    return AIRY_ERR_NOT_SUPPORTED;
#endif
}

/* ============================================================================
 * web_fetch：联网抓取网页内容
 *
 * 主路径：curl 子进程（生产级 HTTPS/TLS + 重定向 + Content-Length 解析，
 *         与 shell_run 共用 builtin_shell_run 超时基础设施）。
 * 降级路径：curl 不可用时回退 network_common 层（仅 http://，commons 网络
 *          抽象暂未实现 TLS，https 需 curl）。
 * ============================================================================ */

/* 解析后的 URL 分量 */
typedef struct {
    char scheme[8];  /* "http" / "https" */
    char host[256];  /* 主机名或 IP */
    int port;        /* 显式端口或协议默认端口 */
    char path[2048]; /* 请求路径（含 query/fragment），空则 "/" */
} builtin_url_t;

/**
 * @brief 解析 URL 为 scheme/host/port/path（仅支持 http/https）
 * @return 0 成功，-1 非法 URL
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
 * @brief 降级路径：network_common 层 HTTP GET（仅 http://）
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

static int web_fetch_tool(const char *params_json, tool_result_t *res)
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

    /* fail-closed：拒绝含 shell 元字符的 URL（防命令注入） */
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
    /* 主路径：curl 子进程。
     * -sS 静默进度但保留错误；-L 跟随重定向；--max-time 防挂死；
     * -w 追加状态标记（单引号保护使 curl 解析 \n 为换行）；-A 声明 UA */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
             "curl -sSL --max-time 30 -A \"AirymaxRT/0.1.1 web_fetch\" "
             "-w '\\n__AIRY_STATUS__:%%{http_code}' '%s'",
             url_str);

    char *out = NULL;
    int exit_code = -1;
    int rc = builtin_shell_run(cmd, &out, &exit_code, 45000, NULL);
    if (rc != 0) {
        res->error = AIRY_STRDUP("Failed to execute web fetch (fork/pipe failed)");
        return AIRY_ERR_EXEC_FAIL;
    }

    /* curl 不可用（sh 找不到命令，退出码 127）→ 降级 network 层（仅 http://） */
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

    /* 提取 HTTP 状态码与响应体（去掉尾部状态标记） */
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
        /* curl 网络/协议错误（DNS/连接/TLS 失败），输出即错误详情 */
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
 * fs_glob：通配符递归列文件（对标 Atom Code GlobTool / Codex find 引导）
 *   参数：pattern（必，支持 * ? 与 ** 段），base（可选，默认 "."）
 *   输出：换行分隔的匹配相对路径；上限 BUILTIN_GLOB_MAX 条
 * ============================================================================ */

#define BUILTIN_GLOB_MAX 2000
#define BUILTIN_GREP_MAX 200
#define BUILTIN_WEBSEARCH_MAX 8

/* 段内通配符匹配：* 匹配任意（不含 '/'），? 匹配单字符 */
static int builtin_glob_seg_match(const char *pat, const char *str)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*')
                pat++;
            if (!*pat)
                return 1;
            for (const char *s = str; *s; s++) {
                if (builtin_glob_seg_match(pat, s))
                    return 1;
            }
            return 0;
        } else if (*pat == '?') {
            if (!*str)
                return 0;
            pat++;
            str++;
        } else {
            if (*pat != *str)
                return 0;
            pat++;
            str++;
        }
    }
    return *str == '\0';
}

/* 递归 glob：base 下按 segs[i..] 匹配；匹配路径写入 out（行分隔，cap 保护） */
static void builtin_glob_impl(const char *base, const char **segs, size_t n, size_t i,
                              char *path, size_t path_len, size_t path_cap, char *out,
                              size_t out_cap, size_t *out_len, size_t *count,
                              size_t max)
{
    if (*count >= max || *out_len >= out_cap - 1)
        return;

    /* 完整目录 = base（path 为空时）或 base/path（跟随累积的相对路径） */
    char full[AIRY_PATH_MAX];
    if (path_len > 0)
        snprintf(full, sizeof(full), "%s/%s", base, path);
    else
        snprintf(full, sizeof(full), "%s", base);

    if (i == n) {
        /* 全部段匹配完成：输出当前路径（文件或目录均可） */
        if (path_len > 0 && path_len + 2 <= out_cap - *out_len) {
            __builtin_memcpy(out + *out_len, path, path_len);
            *out_len += path_len;
            out[(*out_len)++] = '\n';
            out[*out_len] = '\0';
            (*count)++;
        }
        return;
    }

    const char *seg = segs[i];
    const int is_recursive = (strcmp(seg, "**") == 0);

    if (is_recursive) {
        /* ** 匹配 0 层：继续匹配剩余段 */
        builtin_glob_impl(base, segs, n, i + 1, path, path_len, path_cap, out, out_cap,
                          out_len, count, max);
        /* ** 匹配 ≥1 层：进入每个子目录递归（保持 ** 段） */
        DIR *d = opendir(full);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL && *count < max) {
                const char *nm = ent->d_name;
                if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
                    continue;
#ifdef DT_DIR
                int is_dir = (ent->d_type == DT_DIR);
#else
                struct stat st;
                char probe[AIRY_PATH_MAX];
                snprintf(probe, sizeof(probe), "%s/%s", full, nm);
                int is_dir = (stat(probe, &st) == 0 && S_ISDIR(st.st_mode));
#endif
                if (!is_dir)
                    continue;
                size_t need = (path_len ? path_len + 1 : 0) + strlen(nm);
                if (need + 1 >= path_cap)
                    continue;
                char *p = path + path_len;
                if (path_len)
                    *p++ = '/';
                __builtin_memcpy(p, nm, strlen(nm) + 1);
                builtin_glob_impl(base, segs, n, i, path, path_len + (path_len ? 1 : 0) +
                                     strlen(nm), path_cap, out, out_cap, out_len, count,
                                 max);
                if (path_len)
                    path[path_len] = '\0'; /* 恢复 */
            }
            closedir(d);
        }
        return;
    }

    /* 普通段：枚举当前目录，名字匹配 seg 的条目进入下一段 */
    DIR *d = opendir(full);
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && *count < max) {
        const char *nm = ent->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
            continue;
        if (!builtin_glob_seg_match(seg, nm))
            continue;
#ifdef DT_DIR
        int is_dir = (ent->d_type == DT_DIR);
#else
        struct stat st;
        char probe[AIRY_PATH_MAX];
        snprintf(probe, sizeof(probe), "%s/%s", full, nm);
        int is_dir = (stat(probe, &st) == 0 && S_ISDIR(st.st_mode));
#endif
        /* 中间段只进目录；最后一段目录/文件均可 */
        size_t need = (path_len ? path_len + 1 : 0) + strlen(nm);
        if (need + 1 >= path_cap)
            continue;
        char *p = path + path_len;
        if (path_len)
            *p++ = '/';
        __builtin_memcpy(p, nm, strlen(nm) + 1);
        if (i + 1 == n) {
            /* 最后一段：文件与目录都输出 */
            builtin_glob_impl(base, segs, n, i + 1, path, path_len + (path_len ? 1 : 0) +
                                 strlen(nm), path_cap, out, out_cap, out_len, count, max);
        } else if (is_dir) {
            builtin_glob_impl(base, segs, n, i + 1, path, path_len + (path_len ? 1 : 0) +
                                 strlen(nm), path_cap, out, out_cap, out_len, count, max);
        }
        if (path_len)
            path[path_len] = '\0';
    }
    closedir(d);
}

static int fs_glob_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *pat = cJSON_GetObjectItem(root, "pattern");
    if (!cJSON_IsString(pat) || !pat->valuestring || !pat->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: pattern");
        return AIRY_ERR_INVALID_PARAM;
    }
    cJSON *base = cJSON_GetObjectItem(root, "base");
    const char *base_dir =
        (cJSON_IsString(base) && base->valuestring && base->valuestring[0])
            ? base->valuestring
            : ".";

    /* pattern 拆段（按 '/'） */
    const char *segs[64];
    size_t nsegs = 0;
    const char *s = pat->valuestring;
    while (*s) {
        while (*s == '/')
            s++;
        if (!*s)
            break;
        const char *e = s;
        while (*e && *e != '/')
            e++;
        if (nsegs >= 64) {
            res->error = AIRY_STRDUP("pattern too deep (max 64 segments)");
            return AIRY_ERR_INVALID_PARAM;
        }
        char *seg = (char *)AIRY_MALLOC((size_t)(e - s) + 1);
        if (!seg) {
            for (size_t k = 0; k < nsegs; k++)
                AIRY_FREE((void *)segs[k]);
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        __builtin_memcpy(seg, s, (size_t)(e - s));
        seg[e - s] = '\0';
        segs[nsegs++] = seg;
        s = e;
    }
    if (nsegs == 0) {
        res->error = AIRY_STRDUP("pattern is empty after splitting");
        return AIRY_ERR_INVALID_PARAM;
    }

    char *out = (char *)AIRY_CALLOC(BUILTIN_OUTPUT_CAP, 1);
    char *path = (char *)AIRY_MALLOC(AIRY_PATH_MAX);
    if (!out || !path) {
        for (size_t k = 0; k < nsegs; k++)
            AIRY_FREE((void *)segs[k]);
        AIRY_FREE(out);
        AIRY_FREE(path);
        res->error = AIRY_STRDUP("Out of memory");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    size_t out_len = 0, count = 0;
    builtin_glob_impl(base_dir, segs, nsegs, 0, path, 0, AIRY_PATH_MAX, out,
                      BUILTIN_OUTPUT_CAP, &out_len, &count, BUILTIN_GLOB_MAX);

    for (size_t k = 0; k < nsegs; k++)
        AIRY_FREE((void *)segs[k]);
    AIRY_FREE(path);

    if (count == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "No files match pattern '%s' under '%s'",
                 pat->valuestring, base_dir);
        res->error = AIRY_STRDUP(msg);
        res->success = 0;
        res->exit_code = 1;
        return AIRY_OK;
    }
    if (count >= BUILTIN_GLOB_MAX)
        builtin_append_trunc_mark(out, BUILTIN_OUTPUT_CAP, out_len,
                                  "\n[glob truncated: too many matches]");
    res->output = out;
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}

/* ============================================================================
 * fs_grep：正则内容搜索（对标 Atom Code GrepTool / Claude Code rg 引导）
 *   参数：pattern（必，POSIX ERE），path（可选 "."），max_results（可选 200）
 *   输出：relpath:lineno:line 换行分隔；跳过 .git/.venv 等噪音目录与二进制
 * ============================================================================ */

static int builtin_grep_dir(const char *base, const char *root, regex_t *re,
                            const char *glob_filter, int max_results, char *out,
                            size_t out_cap, size_t *out_len, int *count, int *done)
{
    DIR *d = opendir(base);
    if (!d)
        return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && !*done) {
        const char *nm = ent->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
            continue;
        if (strcmp(nm, ".git") == 0 || strcmp(nm, "node_modules") == 0 ||
            strcmp(nm, "target") == 0 || strcmp(nm, ".venv") == 0 ||
            strcmp(nm, "__pycache__") == 0 || strcmp(nm, ".airymaxrt") == 0 ||
            strcmp(nm, "build") == 0 || strcmp(nm, "logs") == 0)
            continue;
        char full[AIRY_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", base, nm);
#ifdef DT_DIR
        if (ent->d_type == DT_DIR) {
            builtin_grep_dir(full, root, re, glob_filter, max_results, out, out_cap,
                             out_len, count, done);
            continue;
        }
        if (ent->d_type != DT_REG)
            continue;
#else
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            builtin_grep_dir(full, root, re, glob_filter, max_results, out, out_cap,
                             out_len, count, done);
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;
#endif
        if (glob_filter && !builtin_glob_seg_match(glob_filter, nm))
            continue;
        if (*count >= max_results)
            break;

        FILE *fp = fopen(full, "rb");
        if (!fp)
            continue;
        char *line = NULL;
        size_t lcap = 0;
        ssize_t llen;
        long lineno = 0;
        while ((llen = getline(&line, &lcap, fp)) >= 0) {
            lineno++;
            if (memchr(line, '\0', (size_t)llen) != NULL)
                break; /* 二进制文件：跳过 */
            /* 去掉尾部换行 */
            size_t tlen = (size_t)llen;
            while (tlen > 0 && (line[tlen - 1] == '\n' || line[tlen - 1] == '\r'))
                tlen--;
            if (regexec(re, line, 0, NULL, 0) == 0) {
                if (*count >= max_results)
                    break;
                /* 相对路径（相对 root） */
                const char *rel = full;
                if (strncmp(root, full, strlen(root)) == 0 && full[strlen(root)] == '/')
                    rel = full + strlen(root) + 1;
                size_t need = strlen(rel) + 16 + tlen + 2;
                if (*out_len + need >= out_cap) {
                    builtin_append_trunc_mark(out, out_cap, *out_len,
                                              "\n[grep truncated: output cap]");
                    *done = 1;
                    break;
                }
                int w = snprintf(out + *out_len, out_cap - *out_len, "%s:%ld:", rel, lineno);
                if (w > 0)
                    *out_len += (size_t)w;
                __builtin_memcpy(out + *out_len, line, tlen);
                *out_len += tlen;
                out[(*out_len)++] = '\n';
                out[*out_len] = '\0';
                (*count)++;
            }
        }
        AIRY_FREE(line);
        fclose(fp);
    }
    closedir(d);
    return 0;
}

static int fs_grep_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *pat = cJSON_GetObjectItem(root, "pattern");
    if (!cJSON_IsString(pat) || !pat->valuestring || !pat->valuestring[0]) {
        res->error = AIRY_STRDUP("Missing string parameter: pattern");
        return AIRY_ERR_INVALID_PARAM;
    }
    cJSON *path = cJSON_GetObjectItem(root, "path");
    const char *dir =
        (cJSON_IsString(path) && path->valuestring && path->valuestring[0])
            ? path->valuestring
            : ".";
    cJSON *gf = cJSON_GetObjectItem(root, "glob");
    const char *glob_filter = (cJSON_IsString(gf) && gf->valuestring) ? gf->valuestring : NULL;
    cJSON *mr = cJSON_GetObjectItem(root, "max_results");
    int max_results = (cJSON_IsNumber(mr) && mr->valueint > 0) ? mr->valueint
                                                               : BUILTIN_GREP_MAX;
    if (max_results > 1000)
        max_results = 1000;

    regex_t re;
    if (regcomp(&re, pat->valuestring, REG_EXTENDED | REG_NOSUB) != 0) {
        res->error = AIRY_STRDUP("Invalid regex pattern");
        return AIRY_ERR_INVALID_PARAM;
    }
    char *out = (char *)AIRY_CALLOC(BUILTIN_OUTPUT_CAP, 1);
    if (!out) {
        regfree(&re);
        res->error = AIRY_STRDUP("Out of memory");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    size_t out_len = 0;
    int count = 0, done = 0;
    builtin_grep_dir(dir, dir, &re, glob_filter, max_results, out, BUILTIN_OUTPUT_CAP,
                     &out_len, &count, &done);
    regfree(&re);

    if (count == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "No matches for pattern '%s' under '%s'",
                 pat->valuestring, dir);
        res->error = AIRY_STRDUP(msg);
        res->success = 0;
        res->exit_code = 1;
        return AIRY_OK;
    }
    res->output = out;
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}

/* ============================================================================
 * fs_edit：精确字符串替换编辑（对标 Codex apply_patch / Claude Code Edit）
 *   参数：path（必），old（必，被替换串），new（必），count（可选 1，替换次数）
 *   输出：替换统计摘要；old 未命中返回明确错误（供 LLM 调整）
 * ============================================================================ */

static int fs_edit_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    cJSON *old = cJSON_GetObjectItem(root, "old");
    cJSON *new = cJSON_GetObjectItem(root, "new");
    cJSON *cnt = cJSON_GetObjectItem(root, "count");
    if (!cJSON_IsString(path) || !path->valuestring || !path->valuestring[0] ||
        !cJSON_IsString(old) || !old->valuestring || !old->valuestring[0] ||
        !cJSON_IsString(new) || !new->valuestring) {
        res->error = AIRY_STRDUP("Missing required string parameter: path/old/new");
        return AIRY_ERR_INVALID_PARAM;
    }
    int max_rep = (cJSON_IsNumber(cnt) && cnt->valueint > 0) ? cnt->valueint : 1;

    FILE *fp = fopen(path->valuestring, "rb");
    if (!fp) {
        char err[512];
        snprintf(err, sizeof(err), "Cannot open file '%s': %s", path->valuestring,
                 strerror(errno));
        res->error = AIRY_STRDUP(err);
        return (errno == ENOENT) ? AIRY_ERR_NOT_FOUND : AIRY_ERR_IO;
    }
    int truncated = 0;
    char *content = builtin_read_all(fp, &truncated);
    fclose(fp);
    if (!content) {
        res->error = AIRY_STRDUP("Failed to read file (I/O error)");
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    /* 统计 old 出现次数（不重叠） */
    size_t olen = strlen(old->valuestring);
    size_t nlen = strlen(new->valuestring);
    size_t content_len = strlen(content);
    int total = 0;
    {
        const char *p = content;
        while ((p = strstr(p, old->valuestring)) != NULL) {
            total++;
            p += olen;
        }
    }
    if (total == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "String not found in '%s': %s", path->valuestring,
                 old->valuestring);
        res->error = AIRY_STRDUP(msg);
        AIRY_FREE(content);
        res->success = 0;
        res->exit_code = 1;
        return AIRY_OK;
    }
    int reps = (total < max_rep) ? total : max_rep;

    /* 计算新大小并重建（从前往后替换 reps 次） */
    size_t new_size = content_len - (size_t)reps * olen + (size_t)reps * nlen;
    char *buf = (char *)AIRY_MALLOC(new_size + 1);
    if (!buf) {
        AIRY_FREE(content);
        res->error = AIRY_STRDUP("Out of memory");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    size_t w = 0, done = 0;
    const char *p = content;
    const char *cur = content;
    while (done < (size_t)reps && (p = strstr(cur, old->valuestring)) != NULL) {
        __builtin_memcpy(buf + w, cur, (size_t)(p - cur));
        w += (size_t)(p - cur);
        __builtin_memcpy(buf + w, new->valuestring, nlen);
        w += nlen;
        cur = p + olen;
        done++;
    }
    if (w < new_size) {
        __builtin_memcpy(buf + w, cur, new_size - w);
        w = new_size;
    }
    buf[w] = '\0';
    AIRY_FREE(content);

    FILE *wfp = fopen(path->valuestring, "wb");
    if (!wfp) {
        char err[512];
        snprintf(err, sizeof(err), "Cannot write file '%s': %s", path->valuestring,
                 strerror(errno));
        res->error = AIRY_STRDUP(err);
        AIRY_FREE(buf);
        return AIRY_ERR_IO;
    }
    size_t wr = fwrite(buf, 1, w, wfp);
    fclose(wfp);
    char ok[512];
    snprintf(ok, sizeof(ok), "Replaced %d occurrence(s) of %zu-byte string in '%s' "
             "(total matches: %d, %zu bytes written)",
             reps, olen, path->valuestring, total, wr);
    AIRY_FREE(buf);
    res->output = AIRY_STRDUP(ok);
    res->success = 1;
    res->exit_code = 0;
    return AIRY_OK;
}

/* ============================================================================
 * web_search：DuckDuckGo HTML 搜索（对标 Atom Code WebSearchTool）
 *   参数：query（必），max_results（可选 8）
 *   输出：标题 / URL / 摘要 分组，行分隔
 * ============================================================================ */

static void builtin_url_encode(const char *in, char *out, size_t out_cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c && o + 3 < out_cap; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
            (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.' || *c == '~') {
            out[o++] = (char)*c;
        } else {
            out[o++] = '%';
            out[o++] = hex[*c >> 4];
            out[o++] = hex[*c & 0xF];
        }
    }
    out[o] = '\0';
}

/* HTML 实体解码（&amp; &lt; &gt; &quot; &#39; 等最小集） */
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

/* 剥除 HTML 标签（就地），随后做实体解码 */
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

/* 通用搜索结果提取：按结果 regex + 摘要 regex 从 html 中提取 max 条，
 * 写入 buf（行分隔："[N] title\n    url\n    snippet\n"）。 */
static int web_search_extract(const char *html, const char *res_pattern,
                              const char *snip_pattern, int max, char *buf,
                              size_t buf_cap, size_t *buf_len, int *count)
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
            /* 摘要紧跟在结果之后时一次命中；否则从匹配段末尾起寻找 */
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
        cur += m[0].rm_eo; /* 相对当前位置推进，避免重复匹配第一项 */
    }
    regfree(&re);
    if (has_s)
        regfree(&re_s);
    return 0;
}

/* Bing 搜索后端（cn.bing.com，国内网络可用；DDG 不可达时降级） */
static int web_search_via_bing(const char *query, int max_results, char *buf,
                               size_t buf_cap, size_t *buf_len, int *count)
{
    char enc[2048];
    builtin_url_encode(query, enc, sizeof(enc));
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
             "curl -sSL --max-time 30 -A \"Mozilla/5.0 (compatible; AirymaxRT/0.1.1 "
             "web_search)\" 'https://www.bing.com/search?q=%s'", enc);
    char *out = NULL;
    int exit_code = -1;
    if (builtin_shell_run(cmd, &out, &exit_code, 45000, NULL) != 0 || !out)
        return -1;
    if (exit_code != 0 || strstr(out, "command not found")) {
        AIRY_FREE(out);
        return -1;
    }
    int rc = web_search_extract(out,
                                "<h2[^>]*><a[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a></h2>",
                                "class=\"b_lineclamp[^\"]*\"[^>]*>(.*?)</p>",
                                max_results, buf, buf_cap, buf_len, count);
    AIRY_FREE(out);
    return rc;
}

static int web_search_tool(const char *params_json, tool_result_t *res)
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
    int max_results = (cJSON_IsNumber(mr) && mr->valueint > 0) ? mr->valueint
                                                               : BUILTIN_WEBSEARCH_MAX;
    if (max_results > BUILTIN_WEBSEARCH_MAX)
        max_results = BUILTIN_WEBSEARCH_MAX;

    char *buf = (char *)AIRY_CALLOC(BUILTIN_OUTPUT_CAP, 1);
    if (!buf) {
        res->error = AIRY_STRDUP("Out of memory");
        return AIRY_ERR_OUT_OF_MEMORY;
    }
    size_t buf_len = 0;
    int count = 0;

    /* 主后端：DuckDuckGo HTML（海外网络）。 */
    {
        char enc[2048];
        builtin_url_encode(q->valuestring, enc, sizeof(enc));
        char cmd[8192];
        snprintf(cmd, sizeof(cmd),
                 "curl -sSL --max-time 30 -A \"Mozilla/5.0 (compatible; "
                 "AirymaxRT/0.1.1 web_search)\" "
                 "'https://html.duckduckgo.com/html/?q=%s'", enc);
        char *out = NULL;
        int exit_code = -1;
        int curl_ok = (builtin_shell_run(cmd, &out, &exit_code, 45000, NULL) == 0 && out &&
                       exit_code == 0 && !strstr(out, "command not found"));
        if (curl_ok) {
            web_search_extract(out,
                               "class=\"result__a\" href=\"([^\"]+)\"[^>]*>([^<]+)</a>",
                               "class=\"result__snippet\"[^>]*>([^<]+)</a>",
                               max_results, buf, BUILTIN_OUTPUT_CAP, &buf_len, &count);
        }
        if (out)
            AIRY_FREE(out);
    }

    /* 降级后端：Bing（cn.bing.com，国内网络可用；DDG 不可达/无结果时） */
    if (count == 0) {
        web_search_via_bing(q->valuestring, max_results, buf, BUILTIN_OUTPUT_CAP,
                            &buf_len, &count);
    }

    if (count == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "No web results for query: %s (DuckDuckGo & Bing unreachable)", q->valuestring);
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

int tool_builtin_is_builtin(const char *executable)
{
    return executable && strncmp(executable, "builtin:", 8) == 0;
}

int tool_builtin_run(const char *tool_id, const char *params_json, tool_result_t *res)
{
    if (!tool_id || !res) {
        return AIRY_ERR_INVALID_PARAM;
    }
    if (strcmp(tool_id, "fs_read") == 0)
        return fs_read_tool(params_json, res);
    if (strcmp(tool_id, "fs_write") == 0)
        return fs_write_tool(params_json, res);
    if (strcmp(tool_id, "fs_list") == 0)
        return fs_list_tool(params_json, res);
    if (strcmp(tool_id, "shell_run") == 0)
        return shell_run_tool(params_json, res);
    if (strcmp(tool_id, "web_fetch") == 0)
        return web_fetch_tool(params_json, res);
#ifndef _WIN32
    if (strcmp(tool_id, "fs_glob") == 0)
        return fs_glob_tool(params_json, res);
    if (strcmp(tool_id, "fs_grep") == 0)
        return fs_grep_tool(params_json, res);
    if (strcmp(tool_id, "fs_edit") == 0)
        return fs_edit_tool(params_json, res);
    if (strcmp(tool_id, "web_search") == 0)
        return web_search_tool(params_json, res);
#endif
    SVC_LOG_ERROR("builtin: unknown builtin tool '%s'", tool_id);
    res->error = AIRY_STRDUP("Unknown builtin tool");
    return AIRY_ERR_EXEC_NOT_FOUND;
}
