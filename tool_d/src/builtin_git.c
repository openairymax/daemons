// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_git.c
 * @brief Built-in tool git domain: git_exec / git_diff / git_apply atomic
 *        git file-modification capability (fork + pipe + poll + waitpid,
 *        supporting stdin data injection).
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

#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#include "tool_builtin_internal.h"

#ifndef _WIN32
/* ============================================================================
 * git_exec / git_diff / git_apply: atomic git file-modification capability
 * (modeled on Codex patch)
 * - git_exec: whitelisted read-only commands (status/diff/log/branch/show/
 *   ls-files/grep etc.), executing git directly via execvp (argv array, no
 *   shell interpretation, avoiding command injection), with timeout truncation.
 * - git_diff: produce a unified diff for the given path (git diff
 *   [--cached] [path]).
 * - git_apply: apply a unified diff to the workspace (git apply [--check] -,
 *   patch fed via stdin).
 * ============================================================================ */

#define BUILTIN_GIT_MAX_ARGS 32

static const char *const g_git_readonly_cmds[] = {
    "status",   "diff",         "log",          "branch",   "show",          "ls-files",
    "ls-tree",  "grep",         "rev-parse",    "blame",    "describe",      "diff-tree",
    "name-rev", "rev-list",     "for-each-ref", "show-ref", "count-objects", "fsck",
    "shortlog", "symbolic-ref", "var",          "version",  "help",          "whatchanged",
    "remote",   "submodule",    "mergetool",    NULL,
};

/**
 * @brief Run a git command with timeout and capture stdout/stderr
 *        (fork + pipe + poll + waitpid)
 *
 * Unlike builtin_shell_run: executes git directly via execvp (argv array, no
 * shell interpretation, avoiding command injection), and supports writing data
 * to the child's stdin (needed by git_apply).
 * @param argv          argv array (argv[0]="git", NULL-terminated)
 * @param stdin_data    Data to write to the child's stdin (may be NULL)
 * @param stdin_len     stdin_data length
 * @param out           Captured output (stdout+stderr merged, AIRY_MALLOC, caller frees)
 * @param exit_code     Process exit code (-1 on timeout)
 * @param out_truncated Whether the output was truncated
 * @return 0 on success, non-zero on failure (fork/pipe/OOM)
 */
static int builtin_git_run(char *const argv[], const char *stdin_data, size_t stdin_len, char **out,
                           int *exit_code, int *out_truncated)
{
    *out = NULL;
    *exit_code = -1;
    if (out_truncated)
        *out_truncated = 0;

    int outfd[2];
    int infd[2];
    if (pipe(outfd) != 0)
        return -1;
    if (pipe(infd) != 0) {
        close(outfd[0]);
        close(outfd[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(outfd[0]);
        close(outfd[1]);
        close(infd[0]);
        close(infd[1]);
        return -1;
    }
    if (pid == 0) {

        close(outfd[0]);
        close(infd[1]);
        dup2(outfd[1], STDOUT_FILENO);
        dup2(outfd[1], STDERR_FILENO);
        dup2(infd[0], STDIN_FILENO);
        close(outfd[1]);
        close(infd[0]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(outfd[1]);
    close(infd[0]);

    /* Parent: write the stdin data to the child first (git_apply's patch),
     * then enter the read loop. git apply only produces output after reading
     * all of stdin, so write-then-read cannot deadlock. */
    if (stdin_data && stdin_len > 0) {
        size_t off = 0;
        while (off < stdin_len) {
            ssize_t n = write(infd[1], stdin_data + off, stdin_len - off);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            off += (size_t)n;
        }
    }
    close(infd[1]);

    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(outfd[0]);
        return -1;
    }
    buf[0] = '\0';

    int wstatus = 0;
    int exited = 0;
    int timed_out = 0;
    int truncated = 0;

    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t deadline_ms =
        (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000 + BUILTIN_SHELL_TIMEOUT_MS;

    for (;;) {
        if (!exited) {
            pid_t w = waitpid(pid, &wstatus, WNOHANG);
            if (w == pid)
                exited = 1;
        }
        if (exited)
            break;

        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        uint64_t now_ms = (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;
        if (now_ms >= deadline_ms) {
            timed_out = 1;
            break;
        }

        struct pollfd pfd = {.fd = outfd[0], .events = POLLIN};
        int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            char chunk[4096];
            ssize_t n = read(outfd[0], chunk, sizeof(chunk));
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

    for (;;) {
        char chunk[4096];
        ssize_t n = read(outfd[0], chunk, sizeof(chunk));
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
    close(outfd[0]);

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

int git_exec_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *args = cJSON_GetObjectItem(root, "command_args");
    if (!cJSON_IsArray(args) || cJSON_GetArraySize(args) < 1) {
        res->error = AIRY_STRDUP("Missing array parameter: command_args");
        return AIRY_ERR_INVALID_PARAM;
    }
    cJSON *cwd = cJSON_GetObjectItem(root, "cwd");
    const char *cwd_str =
        (cJSON_IsString(cwd) && cwd->valuestring && cwd->valuestring[0]) ? cwd->valuestring : NULL;

    cJSON *first = cJSON_GetArrayItem(args, 0);
    if (!cJSON_IsString(first) || !first->valuestring || !first->valuestring[0]) {
        res->error = AIRY_STRDUP("command_args[0] must be a non-empty string subcommand");
        return AIRY_ERR_INVALID_PARAM;
    }
    int allowed = 0;
    for (size_t k = 0; g_git_readonly_cmds[k]; k++) {
        if (strcmp(first->valuestring, g_git_readonly_cmds[k]) == 0) {
            allowed = 1;
            break;
        }
    }
    if (!allowed) {
        char err[512];
        snprintf(err, sizeof(err), "git subcommand '%s' is not in the read-only whitelist",
                 first->valuestring);
        res->error = AIRY_STRDUP(err);
        return AIRY_ERR_INVALID_PARAM;
    }

    int n = cJSON_GetArraySize(args);
    if (n > BUILTIN_GIT_MAX_ARGS) {
        res->error = AIRY_STRDUP("Too many command_args (max 32)");
        return AIRY_ERR_INVALID_PARAM;
    }

    char *argv[BUILTIN_GIT_MAX_ARGS + 4];
    int aidx = 0;
    argv[aidx++] = (char *)"git";
    if (cwd_str) {
        argv[aidx++] = (char *)"-C";
        argv[aidx++] = (char *)cwd_str;
    }
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(args, i);
        if (!cJSON_IsString(it) || !it->valuestring) {
            res->error = AIRY_STRDUP("command_args must contain only strings");
            return AIRY_ERR_INVALID_PARAM;
        }
        argv[aidx++] = it->valuestring;
    }
    argv[aidx] = NULL;

    char *out = NULL;
    int exit_code = -1;
    int rc = builtin_git_run(argv, NULL, 0, &out, &exit_code, NULL);
    if (rc != 0) {
        res->error = AIRY_STRDUP("Failed to execute git (fork/pipe failed)");
        return AIRY_ERR_EXEC_FAIL;
    }
    res->output = out ? out : AIRY_STRDUP("");
    res->success = (exit_code == 0) ? 1 : 0;
    res->exit_code = exit_code;
    if (exit_code != 0) {
        char err[256];
        snprintf(err, sizeof(err), "git exited with code %d", exit_code);
        res->error = AIRY_STRDUP(err);
    }
    return AIRY_OK;
}

int git_diff_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *path = cJSON_GetObjectItem(root, "path");
    cJSON *staged = cJSON_GetObjectItem(root, "staged");
    const char *path_str = (cJSON_IsString(path) && path->valuestring && path->valuestring[0]) ?
                               path->valuestring :
                               NULL;
    int use_staged = cJSON_IsTrue(staged) ? 1 : 0;

    char *argv[8];
    int aidx = 0;
    argv[aidx++] = (char *)"git";
    argv[aidx++] = (char *)"diff";
    if (use_staged)
        argv[aidx++] = (char *)"--cached";
    if (path_str)
        argv[aidx++] = (char *)path_str;
    argv[aidx] = NULL;

    char *out = NULL;
    int exit_code = -1;
    int rc = builtin_git_run(argv, NULL, 0, &out, &exit_code, NULL);
    if (rc != 0) {
        res->error = AIRY_STRDUP("Failed to execute git diff (fork/pipe failed)");
        return AIRY_ERR_EXEC_FAIL;
    }
    res->output = out ? out : AIRY_STRDUP("");
    res->success = (exit_code == 0) ? 1 : 0;
    res->exit_code = exit_code;
    if (exit_code != 0) {
        char err[256];
        snprintf(err, sizeof(err), "git diff exited with code %d", exit_code);
        res->error = AIRY_STRDUP(err);
    }
    return AIRY_OK;
}

int git_apply_tool(const char *params_json, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        res->error = AIRY_STRDUP("Invalid params JSON");
        return AIRY_ERR_PARSE_ERROR;
    });
    cJSON *patch = cJSON_GetObjectItem(root, "patch");
    if (!cJSON_IsString(patch) || !patch->valuestring) {
        res->error = AIRY_STRDUP("Missing string parameter: patch");
        return AIRY_ERR_INVALID_PARAM;
    }
    cJSON *check_only = cJSON_GetObjectItem(root, "check_only");
    int do_check = cJSON_IsTrue(check_only) ? 1 : 0;

    char *argv[8];
    int aidx = 0;
    argv[aidx++] = (char *)"git";
    argv[aidx++] = (char *)"apply";
    if (do_check)
        argv[aidx++] = (char *)"--check";
    argv[aidx++] = (char *)"-";
    argv[aidx] = NULL;

    char *out = NULL;
    int exit_code = -1;
    int rc = builtin_git_run(argv, patch->valuestring, strlen(patch->valuestring), &out, &exit_code,
                             NULL);
    if (rc != 0) {
        res->error = AIRY_STRDUP("Failed to execute git apply (fork/pipe failed)");
        return AIRY_ERR_EXEC_FAIL;
    }
    res->output = out ? out : AIRY_STRDUP("");
    res->success = (exit_code == 0) ? 1 : 0;
    res->exit_code = exit_code;
    if (exit_code != 0) {
        char err[256];
        snprintf(err, sizeof(err), "git apply exited with code %d", exit_code);
        res->error = AIRY_STRDUP(err);
    }
    return AIRY_OK;
}
#endif /* !_WIN32 */
