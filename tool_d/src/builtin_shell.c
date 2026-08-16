// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file builtin_shell.c
 * @brief Built-in tool shell-execution domain: subprocess command execution
 *        with timeout/output truncation (fork + pipe + poll + waitpid) and
 *        the shell_run tool implementation.
 */

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

#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#include "tool_builtin_internal.h"

void builtin_append_trunc_mark(char *buf, size_t cap, size_t len, const char *mark)
{
    size_t mlen = strlen(mark);
    if (len + mlen + 1 <= cap) {
        __builtin_memcpy(buf + len, mark, mlen);
        buf[len + mlen] = '\0';
    } else {
        buf[len] = '\0';
    }
}

#ifndef _WIN32

/**
 * @brief Run a shell command with timeout and capture stdout/stderr
 *        (fork + pipe + poll + waitpid)
 *
 * Replaces popen: commands exceeding timeout_ms get SIGKILLed so tool_d never
 * blocks forever.
 * @param cmd           Command string (interpreted by /bin/sh -c)
 * @param out           Captured output (AIRY_MALLOC, caller frees)
 * @param exit_code     Process exit code (-1 on timeout, 126 on sandbox apply failure)
 * @param timeout_ms    Timeout in ms
 * @param out_truncated Whether the output was truncated
 * @param sandbox       When non-NULL, apply the OS-level sandbox before exec in
 *                      the child (Landlock/seccomp/rlimit, affecting only the
 *                      command process and its descendants, not tool_d itself)
 * @return 0 on success, non-zero on failure (fork/pipe/OOM)
 */
int builtin_shell_run(const char *cmd, char **out, int *exit_code, uint32_t timeout_ms,
                      int *out_truncated, const os_sandbox_cfg_t *sandbox)
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

        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        /* Tool isolation: do not leak external LD_PRELOAD injections (sandbox
         * shims, tracing agents) into the command process.  Injected shims
         * can install seccomp filters or interpose libc calls and crash
         * standard tools such as curl, violating the tool isolation contract.
         * The agentrt runtime itself never relies on LD_PRELOAD, so clearing
         * it is safe and makes subprocess behavior deterministic. */
        unsetenv("LD_PRELOAD");
        unsetenv("LD_AUDIT");
        /* P2 OS-level sandbox: apply after fork, before exec
         * (Landlock/seccomp/rlimit). Apply failure denies execution
         * fail-closed (126 matches the bash convention). */
        if (sandbox && sandbox->mode != OS_SANDBOX_MODE_OFF) {
            if (os_sandbox_apply(sandbox) != 0) {
                _exit(126);
            }
        }
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

    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t deadline_ms = (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000 + timeout_ms;

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

        struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN | POLLHUP};
        int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
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
            } else if (n == 0) {
                /* Child closed its output but is still alive; poll would
                 * otherwise return POLLHUP immediately and busy-spin the
                 * loop until the deadline. */
                usleep(10000);
            }
        }
    }

    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }

    /* Flush the remaining output.  A descendant that inherited the pipe
     * write end (e.g. a backgrounded child) would otherwise keep the
     * blocking read below open forever and wedge the tool daemon, so the
     * drain is bounded: once the flush deadline passes the rest of the
     * output is abandoned. */
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t drain_deadline_ms =
        (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000 + BUILTIN_OUTPUT_DRAIN_MS;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        uint64_t now_ms = (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;
        if (now_ms >= drain_deadline_ms)
            break;

        struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN | POLLHUP};
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0)
            continue;
        if (!(pfd.revents & (POLLIN | POLLHUP)))
            break;
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

#else /* _WIN32 */

#include <windows.h>

/* Append read bytes to the growable capture buffer, mirroring the POSIX
 * version's capacity/truncation policy. */
static void win_capture_append(char **buf, size_t *cap, size_t *len, const char *chunk, DWORD n,
                               int *truncated)
{
    if (len + (size_t)n + 1 > *cap) {
        size_t new_cap = *cap * 2;
        if (new_cap > BUILTIN_OUTPUT_CAP)
            new_cap = BUILTIN_OUTPUT_CAP;
        if (new_cap <= *cap) {
            *truncated = 1;
            return;
        }
        char *nb = (char *)AIRY_REALLOC(*buf, new_cap);
        if (!nb) {
            *truncated = 1;
            return;
        }
        *buf = nb;
        *cap = new_cap;
    }
    if (*len + (size_t)n >= *cap) {
        n = (DWORD)(*cap - *len - 1);
        *truncated = 1;
    }
    __builtin_memcpy(*buf + *len, chunk, (size_t)n);
    *len += (size_t)n;
    (*buf)[*len] = '\0';
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
int builtin_shell_run(const char *cmd, char **out, int *exit_code, uint32_t timeout_ms,
                      int *out_truncated, const os_sandbox_cfg_t *sandbox)
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
    BOOL spawned =
        CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
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
            win_capture_append(&buf, &cap, &len, chunk, n, &truncated);
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
        win_capture_append(&buf, &cap, &len, chunk, n, &truncated);
    }
    CloseHandle(h_read);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (timed_out) {
        const char mark[] = "\n[command timed out after 60s]";
        builtin_append_trunc_mark(buf, cap, len, mark);
        len += sizeof(mark) - 1;
        if (len >= cap)
            len = cap - 1;
        buf[len] = '\0';
        *exit_code = -1;
    } else {
        *exit_code = (int)proc_exit;
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
#endif /* _WIN32 */

int shell_run_tool(const char *params_json, tool_result_t *res)
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
    /* P2 OS-level sandbox: shell_run is enabled by default per environment
     * config (Landlock/seccomp/rlimit on Linux; Windows/macOS resolve to
     * OS_SANDBOX_MODE_OFF, keeping the identical call path). */
    os_sandbox_cfg_t sandbox_cfg;
    os_sandbox_cfg_from_env(&sandbox_cfg);
    char *out = NULL;
    int exit_code = -1;
    int rc = builtin_shell_run(cmd->valuestring, &out, &exit_code, BUILTIN_SHELL_TIMEOUT_MS, NULL,
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
