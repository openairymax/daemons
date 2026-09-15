// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

//
// @file builtin.c
// @brief tool_d built-in basic tool set (real implementations, not stubs):
//   fs_read / fs_write / fs_list / shell_run / web_fetch
//
// Design notes:
// - Built-in tools are an out-of-the-box set of agent base capabilities
//   (mirroring the fs and shell tools of Claude Code / OpenAI Codex),
//   usable after explicit authorization via the daemon_security ACL.
// - All tools take params_json (OpenAI tool_call arguments) and write the
//   result to tool_result_t (output = stdout semantics / error = stderr
//   semantics / exit_code).
// - shell_run really executes commands via popen (agent-side command
//   execution), gated by the upper approval layer (fail-closed ACL).
//
// Security boundary:
// - fs operations and shell execution are real I/O, released only by the
//   approval layer (daemon_security ACL fail-closed); unauthorized tools
//   are always refused.

#include "airy_memory.h"
#include "error.h"

#include "builtin.h"
#include "os_sandbox.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <cjson_helpers.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network_common.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "tool_builtin_internal.h"

int builtin_fs_confine(const char *orig_path, int for_write, char *resolved, size_t resolved_cap,
                       tool_result_t *res)
{
    if (os_sandbox_fs_confine(orig_path, for_write, resolved, resolved_cap) == 0)
        return AIRY_OK;
    char err[512];
    snprintf(err, sizeof(err), "Path escapes workspace sandbox: '%s'", orig_path);
    res->error = AIRY_STRDUP(err);
    return AIRY_ERR_PERMISSION_DENIED;
}

char *builtin_read_all(FILE *fp, int *out_truncated)
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

int tool_builtin_is_builtin(const char *executable)
{
    return executable && strncmp(executable, "builtin:", 8) == 0;
}

uint64_t builtin_deadline_ms(uint32_t timeout_ms)
{
    if (timeout_ms == 0)
        return UINT64_MAX;
#if defined(_WIN32)
    return (uint64_t)GetTickCount64() + timeout_ms;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return UINT64_MAX;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL + timeout_ms;
#endif
}

int builtin_deadline_hit(uint64_t deadline_ms)
{
    if (deadline_ms == UINT64_MAX)
        return 0;
#if defined(_WIN32)
    return GetTickCount64() >= deadline_ms;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    uint64_t now = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    return now >= deadline_ms;
#endif
}

int builtin_scan_noise_dir(const char *name)
{
    static const char *const noise[] = {".git",   "node_modules", "target",       ".venv",
                                        "__pycache__",           ".airymaxrt",   "build",
                                        "logs",  "dist",         ".idea",        ".vscode",
                                        ".cache",                "vendor",       NULL};
    for (int i = 0; noise[i]; i++) {
        if (strcmp(name, noise[i]) == 0)
            return 1;
    }
    return 0;
}

int tool_builtin_run(const char *tool_id, const char *params_json, uint32_t timeout_ms,
                     tool_result_t *res)
{
    if (!tool_id || !res) {
        return AIRY_ERR_INVALID_PARAM;
    }
    if (strcmp(tool_id, "fs_read") == 0)
        return fs_read_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "fs_write") == 0)
        return fs_write_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "fs_list") == 0)
        return fs_list_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "shell_run") == 0)
        return shell_run_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "web_fetch") == 0)
        return web_fetch_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "fs_glob") == 0)
        return fs_glob_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "fs_grep") == 0)
        return fs_grep_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "fs_edit") == 0)
        return fs_edit_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "fs_delete") == 0)
        return fs_delete_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "web_search") == 0)
        return web_search_tool(params_json, timeout_ms, res);
#ifndef _WIN32
    if (strcmp(tool_id, "git_exec") == 0)
        return git_exec_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "git_diff") == 0)
        return git_diff_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "git_apply") == 0)
        return git_apply_tool(params_json, timeout_ms, res);
#endif
    if (strcmp(tool_id, "maths_eval") == 0)
        return maths_eval_tool(params_json, timeout_ms, res);
    if (strcmp(tool_id, "maths_stats") == 0)
        return maths_stats_tool(params_json, timeout_ms, res);
    SVC_LOG_ERROR("builtin: unknown builtin tool '%s'", tool_id);
    res->error = AIRY_STRDUP("Unknown builtin tool");
    return AIRY_ERR_EXEC_NOT_FOUND;
}
