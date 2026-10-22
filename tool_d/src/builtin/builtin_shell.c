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

#include "builtin/builtin_buf_internal.h"
#include "builtin/builtin_exec_internal.h"

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

typedef struct {
    HANDLE rd;
    PROCESS_INFORMATION pi;
    char *buf;
    size_t cap;
    size_t len;
    int truncated;
} sh_win_t;

/* Capture pipe with a non-inheritable read end: descendants of the command
 * process cannot keep the pipe open after the command exits (mirrors the
 * POSIX drain-bound design). */
static int sh_pipe(HANDLE *rd, HANDLE *wr)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(rd, wr, &sa, 0))
        return -1;
    if (!SetHandleInformation(*rd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(*rd);
        CloseHandle(*wr);
        return -1;
    }
    return 0;
}

/* Spawn cmd.exe /S /C <cmd>. Returns 0 on success, 1 when the command could
 * not be started (mirrors POSIX sh 127) and -1 on allocation failure. */
static int sh_spawn(const char *cmd, const char *cwd, HANDLE wr, PROCESS_INFORMATION *pi)
{
    STARTUPINFOA si;
    AIRY_MEMSET(&si, 0, sizeof(si));
    AIRY_MEMSET(pi, 0, sizeof(*pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    size_t cap = strlen(cmd) + 32;
    char *line = (char *)AIRY_MALLOC(cap);
    if (!line)
        return -1;
    /* /S keeps the original quoting of cmd (nested quotes survive), /C runs
     * the command and exits; CREATE_NO_WINDOW keeps the daemon console clean.
     * lpCurrentDirectory runs the child in the task workspace when provided
     * (mirrors the POSIX child chdir) so relative paths resolve against the
     * workspace instead of the daemon's cwd. */
    snprintf(line, cap, "cmd.exe /S /C %s", cmd);
    BOOL spawned = CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                                  (cwd && cwd[0]) ? cwd : NULL, &si, pi);
    AIRY_FREE(line);
    if (!spawned)
        AIRY_MEMSET(pi, 0, sizeof(*pi));
    return spawned ? 0 : 1;
}

/* Move pending child output into the capture buffer. Returns 1 when a chunk
 * was consumed, 0 when the pipe is empty, -1 when the pipe broke. */
static int sh_pump(sh_win_t *w)
{
    DWORD avail = 0;
    if (!PeekNamedPipe(w->rd, NULL, 0, NULL, &avail, NULL) || avail == 0)
        return 0;
    char chunk[4096];
    DWORD n = (avail < (DWORD)sizeof(chunk)) ? avail : (DWORD)sizeof(chunk);
    if (!ReadFile(w->rd, chunk, n, &n, NULL) || n == 0)
        return -1;
    (void)builtin_buf_push(&w->buf, &w->cap, &w->len, chunk, n, &w->truncated);
    return 1;
}

/* Hand the captured output back with the timeout/truncation marks. */
static void sh_out(sh_win_t *w, int timed_out, int proc_exit, char **out, int *exit_code,
                   int *out_truncated)
{
    if (timed_out) {
        builtin_buf_mark(w->buf, w->cap, &w->len, "\n[command timed out after 60s]");
        *exit_code = -1;
    } else {
        *exit_code = proc_exit;
    }
    if (w->truncated)
        builtin_buf_mark(w->buf, w->cap, &w->len, "\n[output truncated at 1MB]");
    if (out_truncated)
        *out_truncated = w->truncated;
    *out = w->buf;
}

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

    sh_win_t w;
    AIRY_MEMSET(&w, 0, sizeof(w));
    HANDLE wr = NULL;
    if (sh_pipe(&w.rd, &wr) != 0)
        return -1;
    int rc = sh_spawn(cmd, cwd, wr, &w.pi);
    CloseHandle(wr); /* the child holds the write end */
    if (rc != 0) {
        CloseHandle(w.rd);
        if (rc < 0)
            return -1;
        *exit_code = 127; /* command not runnable (mirrors POSIX sh 127) */
        return 0;
    }

    w.cap = 4096;
    w.buf = (char *)AIRY_MALLOC(w.cap);
    if (!w.buf) {
        TerminateProcess(w.pi.hProcess, 1);
        CloseHandle(w.pi.hThread);
        CloseHandle(w.pi.hProcess);
        CloseHandle(w.rd);
        return -1;
    }
    w.buf[0] = '\0';

    uint64_t deadline_ms = GetTickCount64() + timeout_ms;
    int timed_out = 0;
    while (WaitForSingleObject(w.pi.hProcess, 100) != WAIT_OBJECT_0) {
        if (GetTickCount64() >= deadline_ms) {
            timed_out = 1;
            break;
        }
        while (sh_pump(&w) > 0)
            ;
    }

    DWORD proc_exit = 0;
    if (timed_out) {
        TerminateProcess(w.pi.hProcess, 1);
        WaitForSingleObject(w.pi.hProcess, INFINITE);
        proc_exit = 1;
    } else {
        GetExitCodeProcess(w.pi.hProcess, &proc_exit);
    }

    /* Drain remaining buffered output after exit (bounded by
     * BUILTIN_OUTPUT_DRAIN_MS, mirroring the POSIX flush loop). */
    uint64_t drain_deadline_ms = GetTickCount64() + BUILTIN_OUTPUT_DRAIN_MS;
    while (GetTickCount64() < drain_deadline_ms) {
        int got = sh_pump(&w);
        if (got < 0)
            break;
        if (got == 0)
            Sleep(10);
    }
    CloseHandle(w.rd);
    CloseHandle(w.pi.hThread);
    CloseHandle(w.pi.hProcess);

    sh_out(&w, timed_out, (int)proc_exit, out, exit_code, out_truncated);
    return 0;
}
#endif /* _WIN32 */

int shell_run_tool(const char *params_json, uint32_t timeout_ms, tool_result_t *res)
{
    CJSON_PARSE_GUARD(root, params_json, {
        return btool_fail(res, AIRY_ERR_PARSE_ERROR, "Invalid params JSON");
    });
    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(cmd) || !cmd->valuestring || !cmd->valuestring[0])
        return btool_fail(res, AIRY_ERR_INVALID_PARAM, "Missing string parameter: command");
    /* Optional working directory: when set, the child process chdir()s into
     * it before executing so relative paths in the command resolve against
     * the task workspace (base.py injects the runner workspace_dir). */
    const char *cwd = builtin_str_opt(root, "cwd", NULL);
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
    if (rc != 0)
        return btool_fail(res, AIRY_ERR_EXEC_FAIL,
                          "Failed to execute command (pipe/process creation failed)");
    return btool_finish(res, out, exit_code, "Command");
}
