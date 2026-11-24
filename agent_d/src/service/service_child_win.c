// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_child_win.c
 * @brief Windows agent runner child transport: CreatePipe + CreateProcessA +
 *        Job Object (whole-tree kill) with CRT fd bridging, plus
 *        PeekNamedPipe-based timed line reads. POSIX counterpart:
 *        service_child_posix.c (0.1.21 P0-1).
 *
 * Why CRT fds: agent_entry_internal_t keeps stdin_fd/stdout_fd as int on every
 * platform. On Windows the pipe HANDLEs are wrapped with _open_osfhandle() so
 * the rest of the service (invoke read/write, destroy) stays identical.
 */

#include "io.h"

#include "agent_service_internal.h"

#ifdef _WIN32

#include <stdint.h>
#include <string.h>
#include <windows.h>

/* commons/utils/io/io.h shadows the UCRT <io.h> through a PUBLIC -I leak, so
 * the CRT prototypes are declared locally (same pattern as file_utils.c). */
int _read(int fd, void *buf, unsigned count);
int _close(int fd);
intptr_t _get_osfhandle(int fd);
int _open_osfhandle(intptr_t osfhandle, int flags);

#define AIRY_O_BINARY 0x8000

/* Windows command lines are capped at 32767 characters; the spec JSON is
 * passed verbatim as one argument, so keep a headroomed fixed buffer. */
#define WIN_CMD_MAX 32768
#define WIN_PATH_BUF 1024
/* Slot table is process-lifetime state; agent_service_create() caps agents far
 * below this, so exhaustion only degrades tree-kill to leader-kill. */
#define WIN_CHILD_SLOT_MAX 4096
#define WIN_KILL_GRACE_MS 2000
#define WIN_READ_SLICE_MS 50

/* ── pid -> process/job handle table (tree-kill support) ─────────────────── */

typedef struct {
    long pid;
    HANDLE proc;
    HANDLE job;
} win_child_slot_t;

static INIT_ONCE g_win_child_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_win_child_lock;
static win_child_slot_t g_win_child_slots[WIN_CHILD_SLOT_MAX];

static BOOL CALLBACK win_child_once_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_win_child_lock);
    return TRUE;
}

static void win_child_lock(void)
{
    InitOnceExecuteOnce(&g_win_child_once, win_child_once_cb, NULL, NULL);
    EnterCriticalSection(&g_win_child_lock);
}

static void win_child_unlock(void)
{
    LeaveCriticalSection(&g_win_child_lock);
}

/* Returns 0 on success, -1 when the table is full. */
static int win_child_register(long pid, HANDLE proc, HANDLE job)
{
    win_child_lock();
    for (int i = 0; i < WIN_CHILD_SLOT_MAX; i++) {
        if (g_win_child_slots[i].pid == 0) {
            g_win_child_slots[i].pid = pid;
            g_win_child_slots[i].proc = proc;
            g_win_child_slots[i].job = job;
            win_child_unlock();
            return 0;
        }
    }
    win_child_unlock();
    return -1;
}

/* Steals the handles for pid; returns 1 when an entry existed. */
static int win_child_take(long pid, HANDLE *proc, HANDLE *job)
{
    int found = 0;
    *proc = NULL;
    *job = NULL;
    win_child_lock();
    for (int i = 0; i < WIN_CHILD_SLOT_MAX; i++) {
        if (g_win_child_slots[i].pid == pid) {
            *proc = g_win_child_slots[i].proc;
            *job = g_win_child_slots[i].job;
            g_win_child_slots[i].pid = 0;
            g_win_child_slots[i].proc = NULL;
            g_win_child_slots[i].job = NULL;
            found = 1;
            break;
        }
    }
    win_child_unlock();
    return found;
}

/* ── command line construction (MSVCRT argv quoting) ─────────────────────── */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} win_cmd_t;

static int cmd_put(win_cmd_t *c, char ch)
{
    if (c->len + 1 >= c->cap)
        return -1;
    c->buf[c->len++] = ch;
    c->buf[c->len] = '\0';
    return 0;
}

static int cmd_putn(win_cmd_t *c, char ch, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (cmd_put(c, ch) != 0)
            return -1;
    }
    return 0;
}

/* Append one argument wrapped in double quotes. Escaping follows the MSVCRT
 * parsing rules so a spec JSON full of quotes/backslashes survives intact:
 *   - n backslashes followed by '"'  -> 2n backslashes + '"' (delimiter);
 *   - n backslashes at end of arg    -> 2n backslashes before closing quote;
 *   - backslashes elsewhere          -> emitted verbatim. */
static int cmd_append_arg(win_cmd_t *c, const char *arg)
{
    if (c->len > 0 && cmd_put(c, ' ') != 0)
        return -1;
    if (cmd_put(c, '"') != 0)
        return -1;

    size_t bs = 0;
    for (const char *p = arg;; p++) {
        if (*p == '\\') {
            bs++;
            continue;
        }
        if (*p == '\0') {
            if (cmd_putn(c, '\\', bs * 2) != 0)
                return -1;
            break;
        }
        if (*p == '"') {
            if (cmd_putn(c, '\\', bs * 2 + 1) != 0)
                return -1;
            if (cmd_put(c, '"') != 0)
                return -1;
            bs = 0;
            continue;
        }
        if (cmd_putn(c, '\\', bs) != 0)
            return -1;
        bs = 0;
        if (cmd_put(c, *p) != 0)
            return -1;
    }
    return cmd_put(c, '"');
}

/* Locate a Python interpreter on PATH. python.exe first (the canonical
 * install), then python3.exe, then the py launcher (needs a "-3" argument). */
static int win_find_python(char *out, size_t cap, int *use_launcher)
{
    static const char *const names[] = {"python.exe", "python3.exe", "py.exe"};
    for (int i = 0; i < 3; i++) {
        DWORD n = SearchPathA(NULL, names[i], NULL, (DWORD)cap, out, NULL);
        if (n > 0 && (size_t)n < cap) {
            *use_launcher = (i == 2);
            return 0;
        }
    }
    return -1;
}

/* ── launch ──────────────────────────────────────────────────────────────── */

static int win_launch(const char *spec, const char *agent_id, const char *exe, int run_python,
                      int use_launcher, long *out_pid, int *out_stdin, int *out_stdout)
{
    char *cmd_buf = (char *)AIRY_MALLOC(WIN_CMD_MAX);
    if (!cmd_buf)
        return -1;
    win_cmd_t cmd = {cmd_buf, 0, WIN_CMD_MAX};
    cmd_buf[0] = '\0';

    int bad = 0;
    bad |= cmd_append_arg(&cmd, exe);
    if (run_python) {
        if (use_launcher)
            bad |= cmd_append_arg(&cmd, "-3");
        bad |= cmd_append_arg(&cmd, "-m");
        bad |= cmd_append_arg(&cmd, "airymax_agents.runner");
    }
    bad |= cmd_append_arg(&cmd, "--spec");
    bad |= cmd_append_arg(&cmd, spec);
    if (bad) {
        AIRY_FREE(cmd_buf);
        SVC_LOG_WARN("Agent child command line too long: agent_id=%s", agent_id);
        return -1;
    }

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    HANDLE err_h = INVALID_HANDLE_VALUE;
    /* Declared before the first goto so the done: cleanup never reads an
     * uninitialised handle on the early-failure paths. */
    HANDLE job = NULL;
    HANDLE orphan_job = NULL;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    DWORD win_err = 0;
    int fd_in = -1, fd_out = -1;
    int ok = 0;

    if (!CreatePipe(&in_r, &in_w, &sa, 0))
        goto done;
    if (!CreatePipe(&out_r, &out_w, &sa, 0))
        goto done;
    /* Parent ends must not be inherited by the child. */
    if (!SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0))
        goto done;

    /* stderr -> <airy_log_dir>/agent_<id>.log, same file the POSIX transport
     * uses, so log retention (agent_child_logs_prune) covers both. */
    char log_path[AIRY_PATH_MAX];
    agent_child_log_path(agent_id, log_path, sizeof(log_path));
    err_h = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (err_h == INVALID_HANDLE_VALUE)
        SVC_LOG_WARN("Agent child log open failed: agent_id=%s, path=%s", agent_id, log_path);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = (err_h != INVALID_HANDLE_VALUE) ? err_h : GetStdHandle(STD_ERROR_HANDLE);

    /* CREATE_SUSPENDED: assign to the job object before the child can fork
     * grandchildren, so KILL_ON_JOB_CLOSE always covers the whole tree. */
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (!CreateProcessA(NULL, cmd_buf, NULL, NULL, TRUE, flags, NULL, NULL, &si, &pi)) {
        win_err = GetLastError();
        goto done;
    }

    CloseHandle(in_r);
    in_r = NULL;
    CloseHandle(out_w);
    out_w = NULL;
    if (err_h != INVALID_HANDLE_VALUE) {
        CloseHandle(err_h);
        err_h = INVALID_HANDLE_VALUE;
    }

    HANDLE job_tmp = CreateJobObjectA(NULL, NULL);
    if (job_tmp) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        ZeroMemory(&jeli, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job_tmp, JobObjectExtendedLimitInformation, &jeli,
                                   sizeof(jeli)) &&
            AssignProcessToJobObject(job_tmp, pi.hProcess)) {
            job = job_tmp;
        } else {
            CloseHandle(job_tmp);
        }
    }

    if (win_child_register((long)pi.dwProcessId, pi.hProcess, job) == 0) {
        pi.hProcess = NULL; /* ownership handed to the table */
    } else {
        /* Table full: the child still runs, but only the leader can be killed.
         * The job handle must stay open on the success path — closing it would
         * trigger KILL_ON_JOB_CLOSE and terminate the child we just started —
         * so it is kept for the lifetime of this process. Only when this
         * launch is aborted below is it safe to release. */
        orphan_job = job;
        job = NULL;
        SVC_LOG_WARN("Agent child table full, tree-kill degraded: agent_id=%s", agent_id);
    }

    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        win_err = GetLastError();
        SVC_LOG_WARN("Agent child resume failed: agent_id=%s, err=%lu", agent_id,
                     (unsigned long)win_err);
        goto done;
    }

    fd_in = _open_osfhandle((intptr_t)in_w, AIRY_O_BINARY);
    if (fd_in < 0)
        goto done;
    in_w = NULL; /* fd owns the handle now */
    fd_out = _open_osfhandle((intptr_t)out_r, AIRY_O_BINARY);
    if (fd_out < 0)
        goto done;
    out_r = NULL;

    *out_pid = (long)pi.dwProcessId;
    *out_stdin = fd_in;
    *out_stdout = fd_out;
    ok = 1;

    /* 2026-08-25：spawn 成功后收敛 agent 子进程日志数量（保留最近 64 个）。 */
    agent_child_logs_prune();

done:
    if (!ok) {
        int kill_in = fd_in, kill_out = fd_out;
        long dead_pid = (long)pi.dwProcessId;
        fd_in = -1;
        fd_out = -1;
        if (dead_pid > 0) {
            agent_kill_and_reap(&dead_pid, &kill_in, &kill_out);
        } else {
            if (kill_in >= 0)
                _close(kill_in);
            if (kill_out >= 0)
                _close(kill_out);
        }
        if (win_err != 0)
            SVC_LOG_WARN("Agent child start failed: agent_id=%s, exe=%s, err=%lu", agent_id, exe,
                         (unsigned long)win_err);
    }
    if (pi.hThread)
        CloseHandle(pi.hThread);
    if (pi.hProcess)
        CloseHandle(pi.hProcess);
    if (in_r)
        CloseHandle(in_r);
    if (in_w)
        CloseHandle(in_w);
    if (out_r)
        CloseHandle(out_r);
    if (out_w)
        CloseHandle(out_w);
    if (err_h != INVALID_HANDLE_VALUE)
        CloseHandle(err_h);
    /* Aborted launch with a full slot table: the job stayed out of the table,
     * so release it here (KILL_ON_JOB_CLOSE is harmless now that the child is
     * being terminated). On the success path it is intentionally kept. */
    if (!ok && orphan_job)
        CloseHandle(orphan_job);
    AIRY_FREE(cmd_buf);
    return ok ? 0 : -1;
}

int agent_spawn_child(const char *spec, const char *agent_id, long *out_pid, int *out_stdin,
                      int *out_stdout)
{
    if (!spec || !agent_id || !out_pid || !out_stdin || !out_stdout)
        return -1;
    *out_pid = -1;
    *out_stdin = -1;
    *out_stdout = -1;

    char lang[16] = {0};
    agent_spec_language(spec, lang, sizeof(lang));

    if (strcmp(lang, "rust") == 0) {
        char rust_path[AIRY_PATH_MAX] = {0};
        agent_spec_rust_binary(spec, rust_path, sizeof(rust_path));
        if (rust_path[0] != '\0') {
            if (win_launch(spec, agent_id, rust_path, 0, 0, out_pid, out_stdin, out_stdout) == 0)
                return 0;
            SVC_LOG_WARN("Rust agent launch failed, fallback to python: binary=%s, agent_id=%s",
                         rust_path, agent_id);
        } else {
            SVC_LOG_WARN("Rust agent binary path empty, fallback to python: agent_id=%s", agent_id);
        }
    }

    char py_path[WIN_PATH_BUF] = {0};
    int use_launcher = 0;
    if (win_find_python(py_path, sizeof(py_path), &use_launcher) != 0) {
        SVC_LOG_WARN("No python interpreter found (python.exe/python3.exe/py.exe): agent_id=%s",
                     agent_id);
        return -1;
    }
    return win_launch(spec, agent_id, py_path, 1, use_launcher, out_pid, out_stdin, out_stdout);
}

/* ── timed read / terminate ──────────────────────────────────────────────── */

int agent_read_line_timeout_ex(int fd, char *buf, size_t buf_size, int timeout_s,
                               airy_cancel_token_t *token)
{
    if (!buf || buf_size < 2 || fd < 0)
        return -1;
    intptr_t oh = _get_osfhandle(fd);
    if (oh == -1)
        return -1;
    HANDLE pipe = (HANDLE)oh;

    uint64_t deadline_ms = airy_time_ms() + (uint64_t)(timeout_s > 0 ? timeout_s : 300) * 1000U;
    size_t pos = 0;
    while (pos < buf_size - 1) {
        if (token && airy_cancel_token_is_canceled(token))
            return -2;
        uint64_t now = airy_time_ms();
        if (now >= deadline_ms)
            return -1;

        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL))
            return -1;
        if (avail == 0) {
            uint64_t remain = deadline_ms - now;
            uint32_t slice = WIN_READ_SLICE_MS;
            if (remain < (uint64_t)slice)
                slice = (uint32_t)(remain > 0 ? remain : 1);
            airy_sleep_ms(slice);
            continue;
        }

        int n = _read(fd, buf + pos, 1);
        if (n <= 0)
            return -1;
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            return 0;
        }
        pos++;
    }
    buf[buf_size - 1] = '\0';
    return 1;
}

int agent_read_line_timeout(int fd, char *buf, size_t buf_size, int timeout_s)
{
    return agent_read_line_timeout_ex(fd, buf, buf_size, timeout_s, NULL);
}

void agent_kill_and_reap(long *pid_ptr, int *stdin_ptr, int *stdout_ptr)
{
    if (!pid_ptr || *pid_ptr <= 0)
        return;
    long pid = *pid_ptr;
    *pid_ptr = -1;

    HANDLE proc = NULL;
    HANDLE job = NULL;
    if (win_child_take(pid, &proc, &job)) {
        if (job) {
            TerminateJobObject(job, 1);
            if (proc)
                WaitForSingleObject(proc, WIN_KILL_GRACE_MS);
            CloseHandle(job);
        } else if (proc) {
            TerminateProcess(proc, 1);
            WaitForSingleObject(proc, WIN_KILL_GRACE_MS);
        }
        if (proc)
            CloseHandle(proc);
    } else {
        /* Not in the table (table full or already reaped): best-effort
         * leader termination. */
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid);
        if (h) {
            TerminateProcess(h, 1);
            WaitForSingleObject(h, WIN_KILL_GRACE_MS);
            CloseHandle(h);
        }
    }

    if (stdin_ptr && *stdin_ptr >= 0) {
        _close(*stdin_ptr);
        *stdin_ptr = -1;
    }
    if (stdout_ptr && *stdout_ptr >= 0) {
        _close(*stdout_ptr);
        *stdout_ptr = -1;
    }
}

#else /* !_WIN32 */

/* Avoid an empty translation unit under -Wpedantic on POSIX. */
typedef int agent_child_win_unused_t;

#endif /* _WIN32 */
