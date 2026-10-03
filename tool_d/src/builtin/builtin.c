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

#include "builtin/tool_builtin_internal.h"

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

#ifndef _WIN32
/* Spawn child_fn with stdout+stderr merged onto outfd[1] and stdin from
 * infd[0], clearing the daemon's loader environment in the child; the parent
 * keeps outfd[0] for reading and *pid for reaping. Returns 0 on success. */
static int capture_spawn(builtin_child_fn child_fn, void *child_ctx, int outfd[2], int infd[2],
                         pid_t *pid)
{
    if (pipe(outfd) != 0)
        return -1;
    if (pipe(infd) != 0) {
        close(outfd[0]);
        close(outfd[1]);
        return -1;
    }
    *pid = fork();
    if (*pid < 0) {
        close(outfd[0]);
        close(outfd[1]);
        close(infd[0]);
        close(infd[1]);
        return -1;
    }
    if (*pid == 0) {
        close(outfd[0]);
        close(infd[1]);
        dup2(outfd[1], STDOUT_FILENO);
        dup2(outfd[1], STDERR_FILENO);
        dup2(infd[0], STDIN_FILENO);
        close(outfd[1]);
        close(infd[0]);
        /* Tool isolation contract: never leak the daemon's loader environment
         * into a spawned tool. The airymaxrt launcher exports $AIRY_HOME/lib
         * through LD_LIBRARY_PATH for the runtime's own binaries; a tool that
         * loads those ABI-mismatched .so files aborts (e.g. curl: "libcurl.so.4:
         * no version information available") and loses networking. Injected
         * LD_PRELOAD/LD_AUDIT shims are cleared for the same reason. The daemon
         * keeps its own environment; only the tool and its descendants are
         * cleaned. */
        unsetenv("LD_PRELOAD");
        unsetenv("LD_AUDIT");
        unsetenv("LD_LIBRARY_PATH");
        child_fn(child_ctx);
        _exit(127);
    }
    close(outfd[1]);
    close(infd[0]);
    return 0;
}

/* Feed the child's stdin to completion (git_apply's patch), retrying EINTR so
 * a slow consumer cannot lose a partial write. */
static void capture_feed(int wfd, const char *data, size_t len)
{
    if (!data || len == 0)
        return;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(wfd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)n;
    }
}

/* Pump the child's merged output until it is reaped or the deadline passes.
 * Returns 1 once reaped (wstatus valid), 0 on timeout or when the output cap
 * stops further reads; *timed_out distinguishes the deadline case. */
static int capture_loop(pid_t pid, int rfd, uint32_t timeout_ms, int *wstatus, int *timed_out,
                        char **buf, size_t *cap, size_t *len, int *truncated)
{
    *timed_out = 0;
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t deadline_ms = (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000 + timeout_ms;

    for (;;) {
        if (waitpid(pid, wstatus, WNOHANG) == pid)
            return 1;

        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        uint64_t now_ms = (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;
        if (now_ms >= deadline_ms) {
            *timed_out = 1;
            return 0;
        }

        struct pollfd pfd = {.fd = rfd, .events = POLLIN | POLLHUP};
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0 || !(pfd.revents & (POLLIN | POLLHUP)))
            continue;
        char chunk[4096];
        ssize_t n = read(rfd, chunk, sizeof(chunk));
        if (n > 0) {
            if (!builtin_buf_push(buf, cap, len, chunk, (size_t)n, truncated))
                return 0;
        } else if (n == 0) {
            /* Child closed its output but is still alive; poll would otherwise
             * return POLLHUP immediately and busy-spin until the deadline. */
            usleep(10000);
        }
    }
}

/* Bounded tail drain: a descendant holding the pipe write end (e.g. a
 * backgrounded child) must not wedge the daemon forever, so once the flush
 * deadline passes the rest of the output is abandoned. */
static void capture_drain(int rfd, char **buf, size_t *cap, size_t *len, int *truncated)
{
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t drain_deadline_ms =
        (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000 + BUILTIN_OUTPUT_DRAIN_MS;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        uint64_t now_ms = (uint64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;
        if (now_ms >= drain_deadline_ms)
            return;

        struct pollfd pfd = {.fd = rfd, .events = POLLIN | POLLHUP};
        if (poll(&pfd, 1, 100) <= 0)
            continue;
        if (!(pfd.revents & (POLLIN | POLLHUP)))
            return;
        char chunk[4096];
        ssize_t n = read(rfd, chunk, sizeof(chunk));
        if (n <= 0)
            return;
        if (!builtin_buf_push(buf, cap, len, chunk, (size_t)n, truncated))
            return;
    }
}

/* Resolve the exit code and append the timeout/truncation notices. */
static void capture_finish(int timed_out, int exited, int wstatus, int truncated, char *buf,
                           size_t cap, size_t *len, int *exit_code)
{
    if (timed_out) {
        builtin_buf_mark(buf, cap, len, "\n[command timed out after 60s]");
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
    if (truncated)
        builtin_buf_mark(buf, cap, len, "\n[output truncated at 1MB]");
}

int builtin_proc_capture(builtin_child_fn child_fn, void *child_ctx, const char *stdin_data,
                         size_t stdin_len, uint32_t timeout_ms, char **out, int *exit_code,
                         int *out_truncated)
{
    *out = NULL;
    *exit_code = -1;
    if (out_truncated)
        *out_truncated = 0;

    int outfd[2];
    int infd[2];
    pid_t pid = -1;
    if (capture_spawn(child_fn, child_ctx, outfd, infd, &pid) != 0)
        return -1;

    capture_feed(infd[1], stdin_data, stdin_len);
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
    int timed_out = 0;
    int truncated = 0;
    int exited =
        capture_loop(pid, outfd[0], timeout_ms, &wstatus, &timed_out, &buf, &cap, &len, &truncated);
    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }

    capture_drain(outfd[0], &buf, &cap, &len, &truncated);
    close(outfd[0]);

    capture_finish(timed_out, exited, wstatus, truncated, buf, cap, &len, exit_code);
    if (out_truncated)
        *out_truncated = truncated;
    *out = buf;
    return 0;
}
#endif /* !_WIN32 */

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
    if (strcmp(tool_id, "maths_plot") == 0)
        return maths_plot_tool(params_json, timeout_ms, res);
    SVC_LOG_ERROR("builtin: unknown builtin tool '%s'", tool_id);
    res->error = AIRY_STRDUP("Unknown builtin tool");
    return AIRY_ERR_EXEC_NOT_FOUND;
}
