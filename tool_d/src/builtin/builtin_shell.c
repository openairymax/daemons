// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_shell.c
 * @brief Built-in tool shell-execution domain: subprocess command execution
 *        with timeout/output truncation (over the shared process-capture
 *        engine) and the shell_run tool implementation.
 */

#ifndef _WIN32
#include <unistd.h>
#endif

#include "builtin/tool_builtin_internal.h"

void builtin_append_trunc_mark(char *buf, size_t cap, size_t len, const char *mark)
{
    size_t mlen = strlen(mark);
    if (len + mlen + 1 <= cap) {
        AIRY_MEMCPY(buf + len, mark, mlen);
        buf[len + mlen] = '\0';
    } else {
        buf[len] = '\0';
    }
}

int builtin_buf_push(char **buf, size_t *cap, size_t *len, const char *chunk, size_t n,
                     int *truncated)
{
    if (*len + n + 1 > *cap) {
        size_t new_cap = *cap * 2;
        if (new_cap > BUILTIN_OUTPUT_CAP)
            new_cap = BUILTIN_OUTPUT_CAP;
        if (new_cap <= *cap) {
            *truncated = 1;
            return 0;
        }
        char *nb = (char *)AIRY_REALLOC(*buf, new_cap);
        if (!nb) {
            *truncated = 1;
            return 0;
        }
        *buf = nb;
        *cap = new_cap;
    }
    if (*len + n >= *cap) {
        n = *cap - *len - 1;
        *truncated = 1;
    }
    AIRY_MEMCPY(*buf + *len, chunk, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

void builtin_buf_mark(char *buf, size_t cap, size_t *len, const char *mark)
{
    builtin_append_trunc_mark(buf, cap, *len, mark);
    *len += strlen(mark);
    if (*len >= cap)
        *len = cap - 1;
    buf[*len] = '\0';
}

#ifndef _WIN32

/* Child-side setup for the shell runner: chdir into the task workspace, apply
 * the OS-level sandbox (fail-closed), then exec /bin/sh -c. Exit codes mirror
 * the shell conventions (127 not found, 126 not executable). */
typedef struct {
    const char *cmd;
    const char *cwd;
    const os_sandbox_cfg_t *sandbox;
} shell_ctx_t;

static void shell_spawn(void *ctx)
{
    shell_ctx_t *c = (shell_ctx_t *)ctx;
    if (c->cwd && c->cwd[0] && chdir(c->cwd) != 0)
        _exit(127);
    if (c->sandbox && c->sandbox->mode != OS_SANDBOX_MODE_OFF) {
        if (os_sandbox_apply(c->sandbox) != 0)
            _exit(126);
    }
    execl("/bin/sh", "sh", "-c", c->cmd, (char *)NULL);
    _exit(127);
}

/**
 * @brief Run a shell command with timeout and capture stdout/stderr
 *
 * Thin wrapper over the shared process-capture engine: the fork/pipe/poll/
 * waitpid ritual, timeout enforcement and output cap live in
 * builtin_proc_capture, while the child-side setup (workspace chdir + sandbox
 * + /bin/sh -c) is the shell_spawn strategy callback. Commands exceeding
 * timeout_ms get SIGKILLed so tool_d never blocks forever.
 * @param cmd           Command string (interpreted by /bin/sh -c)
 * @param cwd           Optional working directory for the child process
 *                      (NULL/empty keeps tool_d's cwd; the child chdir()s
 *                      into it before exec so relative paths in the command
 *                      resolve against the task workspace, mirroring git -C)
 * @param out           Captured output (AIRY_MALLOC, caller frees)
 * @param exit_code     Process exit code (-1 on timeout, 126 on sandbox apply failure)
 * @param timeout_ms    Timeout in ms
 * @param out_truncated Whether the output was truncated
 * @param sandbox       When non-NULL, apply the OS-level sandbox before exec in
 *                      the child (Landlock/seccomp/rlimit, affecting only the
 *                      command process and its descendants, not tool_d itself)
 * @return 0 on success, non-zero on failure (fork/pipe/OOM)
 */
int builtin_shell_run(const char *cmd, const char *cwd, char **out, int *exit_code,
                      uint32_t timeout_ms, int *out_truncated, const os_sandbox_cfg_t *sandbox)
{
    shell_ctx_t c = {cmd, cwd, sandbox};
    return builtin_proc_capture(shell_spawn, &c, NULL, 0, timeout_ms, out, exit_code,
                                out_truncated);
}

#else /* _WIN32 */

#include <windows.h>

/**
 * @brief Run a shell command with timeout and capture stdout/stderr
 *        (Windows: CreateProcess + anonymous pipe)
 *
 * Semantics match the POSIX version: commands exceeding timeout_ms are
 * terminated so tool_d never blocks forever; output is capped at
 * BUILTIN_OUTPUT_CAP. The command is executed by cmd.exe /S /C (quoting
 * preserved), on Windows the OS-level sandbox is unavailable so
 * os_sandbox_cfg_t is ignored (mode is always OFF).
 */
int builtin_shell_run(const char *cmd, const char *cwd, char **out, int *exit_code,
                      uint32_t timeout_ms, int *out_truncated, const os_sandbox_cfg_t *sandbox)
{
    (void)sandbox; /* Windows has no OS-level sandbox (mode is always OFF) */
    *out = NULL;
    *exit_code = -1;
    if (out_truncated)
        *out_truncated = 0;

    HANDLE h_read = NULL;
    HANDLE h_write = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&h_read, &h_write, &sa, 0))
        return -1;
    /* Non-inheritable read end: descendants of the command process cannot
     * keep the pipe open after the command exits (mirrors the POSIX
     * drain-bound design). */
    if (!SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(h_read);
        CloseHandle(h_write);
        return -1;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    __builtin_memset(&si, 0, sizeof(si));
    __builtin_memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_write;
    si.hStdError = h_write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    size_t cmd_len = strlen(cmd);
    char *line = (char *)AIRY_MALLOC(cmd_len + 32);
    if (!line) {
        CloseHandle(h_read);
        CloseHandle(h_write);
        return -1;
    }
    /* /S keeps the original quoting of cmd (nested quotes survive), /C runs
     * the command and exits; CREATE_NO_WINDOW keeps the daemon console clean. */
    snprintf(line, cmd_len + 32, "cmd.exe /S /C %s", cmd);
    /* lpCurrentDirectory: run in the task workspace when provided (mirrors
     * the POSIX child chdir), so relative paths resolve against the
     * workspace instead of the daemon's cwd. */
    BOOL spawned =
        CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                       (cwd && cwd[0]) ? cwd : NULL, &si, &pi);
    AIRY_FREE(line);
    CloseHandle(h_write); /* the child holds the write end */
    if (!spawned) {
        CloseHandle(h_read);
        *exit_code = 127; /* command not runnable (mirrors POSIX sh 127) */
        return 0;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)AIRY_MALLOC(cap);
    if (!buf) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(h_read);
        return -1;
    }
    buf[0] = '\0';

    int timed_out = 0;
    int truncated = 0;
    uint64_t deadline_ms = GetTickCount64() + timeout_ms;

    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0)
            break;
        if (GetTickCount64() >= deadline_ms) {
            timed_out = 1;
            break;
        }
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h_read, NULL, 0, NULL, &avail, NULL) || avail == 0)
                break;
            char chunk[4096];
            DWORD n = (avail < (DWORD)sizeof(chunk)) ? avail : (DWORD)sizeof(chunk);
            if (!ReadFile(h_read, chunk, n, &n, NULL) || n == 0)
                break;
            builtin_buf_push(&buf, &cap, &len, chunk, n, &truncated);
        }
    }

    DWORD proc_exit = 0;
    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        proc_exit = 1;
    } else {
        GetExitCodeProcess(pi.hProcess, &proc_exit);
    }

    /* Drain remaining buffered output after exit (bounded by
     * BUILTIN_OUTPUT_DRAIN_MS, mirroring the POSIX flush loop). */
    uint64_t drain_deadline_ms = GetTickCount64() + BUILTIN_OUTPUT_DRAIN_MS;
    for (;;) {
        if (GetTickCount64() >= drain_deadline_ms)
            break;
        DWORD avail = 0;
        if (!PeekNamedPipe(h_read, NULL, 0, NULL, &avail, NULL) || avail == 0) {
            Sleep(10);
            continue;
        }
        char chunk[4096];
        DWORD n = (avail < (DWORD)sizeof(chunk)) ? avail : (DWORD)sizeof(chunk);
        if (!ReadFile(h_read, chunk, n, &n, NULL) || n == 0)
            break;
        builtin_buf_push(&buf, &cap, &len, chunk, n, &truncated);
    }
    CloseHandle(h_read);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (timed_out) {
        builtin_buf_mark(buf, cap, &len, "\n[command timed out after 60s]");
        *exit_code = -1;
    } else {
        *exit_code = (int)proc_exit;
    }
    if (truncated)
        builtin_buf_mark(buf, cap, &len, "\n[output truncated at 1MB]");
    if (out_truncated)
        *out_truncated = truncated;
    *out = buf;
    return 0;
}
#endif /* _WIN32 */

int shell_run_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
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
    /* Optional working directory: when set, the child process chdir()s into
     * it before executing so relative paths in the command resolve against
     * the task workspace (base.py injects the runner workspace_dir). */
    cJSON *cwd_item = cJSON_GetObjectItem(root, "cwd");
    const char *cwd = (cJSON_IsString(cwd_item) && cwd_item->valuestring && cwd_item->valuestring[0])
                          ? cwd_item->valuestring
                          : NULL;
    /* P2 OS-level sandbox: shell_run is enabled by default per environment
     * config (Landlock/seccomp/rlimit on Linux; Windows/macOS resolve to
     * OS_SANDBOX_MODE_OFF, keeping the identical call path). */
    os_sandbox_cfg_t sandbox_cfg;
    os_sandbox_cfg_from_env(&sandbox_cfg);
    char *out = NULL;
    int exit_code = -1;
    int rc = builtin_shell_run(cmd->valuestring, cwd, &out, &exit_code,
                               timeout_ms ? timeout_ms : BUILTIN_SHELL_TIMEOUT_MS, NULL,
                               &sandbox_cfg);
    if (rc != 0) {
        res->error = AIRY_STRDUP("Failed to execute command (pipe/process creation failed)");
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
}
