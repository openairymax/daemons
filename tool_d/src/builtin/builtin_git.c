// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_git.c
 * @brief Built-in tool git domain: git_exec / git_diff / git_apply atomic
 *        git file-modification capability (over the shared process-capture
 *        engine, supporting stdin data injection).
 */

#ifndef _WIN32
#include <unistd.h>
#endif

#include "builtin/tool_builtin_internal.h"

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

/* Child-side setup for the git runners: exec git directly with the argv array
 * (no shell interpretation, avoiding command injection). */
typedef struct {
    char *const *argv;
} git_ctx_t;

static void git_spawn(void *ctx)
{
    git_ctx_t *c = (git_ctx_t *)ctx;
    execvp(c->argv[0], c->argv);
    _exit(127);
}

/**
 * @brief Run a git command with timeout and capture stdout/stderr
 *
 * Thin wrapper over the shared process-capture engine: the fork/pipe/poll/
 * waitpid ritual, timeout and output cap live in builtin_proc_capture, while
 * the child-side setup (execvp of the argv array) is the git_spawn strategy
 * callback. Unlike builtin_shell_run there is no shell interpretation and the
 * child may be fed stdin data (git_apply's patch).
 * @param argv          argv array (argv[0]="git", NULL-terminated)
 * @param stdin_data    Data to write to the child's stdin (may be NULL)
 * @param stdin_len     stdin_data length
 * @param out           Captured output (stdout+stderr merged, AIRY_MALLOC, caller frees)
 * @param exit_code     Process exit code (-1 on timeout)
 * @param out_truncated Whether the output was truncated
 * @return 0 on success, non-zero on failure (fork/pipe/OOM)
 */
static int builtin_git_run(char *const argv[], const char *stdin_data, size_t stdin_len, char **out,
                           int *exit_code, int *out_truncated, uint32_t timeout_ms)
{
    git_ctx_t c = {argv};
    return builtin_proc_capture(git_spawn, &c, stdin_data, stdin_len,
                                timeout_ms ? timeout_ms : BUILTIN_SHELL_TIMEOUT_MS, out, exit_code,
                                out_truncated);
}

int git_exec_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
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
    int rc = builtin_git_run(argv, NULL, 0, &out, &exit_code, NULL, timeout_ms);
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

int git_diff_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
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
    int rc = builtin_git_run(argv, NULL, 0, &out, &exit_code, NULL, timeout_ms);
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

int git_apply_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
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
                             NULL, timeout_ms);
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
