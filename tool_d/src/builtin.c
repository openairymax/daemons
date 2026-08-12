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
#include "os_sandbox.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
#include <cjson_helpers.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#include "tool_builtin_internal.h"

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
    if (strcmp(tool_id, "git_exec") == 0)
        return git_exec_tool(params_json, res);
    if (strcmp(tool_id, "git_diff") == 0)
        return git_diff_tool(params_json, res);
    if (strcmp(tool_id, "git_apply") == 0)
        return git_apply_tool(params_json, res);
#endif
    SVC_LOG_ERROR("builtin: unknown builtin tool '%s'", tool_id);
    res->error = AIRY_STRDUP("Unknown builtin tool");
    return AIRY_ERR_EXEC_NOT_FOUND;
}
