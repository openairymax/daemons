// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
//
// @file builtin.c
// @brief tool_d 内置基础工具集（真实实现，非桩）：
//   fs_read / fs_write / fs_list / shell_run
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

#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <poll.h>
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
    SVC_LOG_ERROR("builtin: unknown builtin tool '%s'", tool_id);
    res->error = AIRY_STRDUP("Unknown builtin tool");
    return AIRY_ERR_EXEC_NOT_FOUND;
}
