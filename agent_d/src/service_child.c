// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_child.c
 * @brief Agent service child-process communication domain: runner child
 *        fork / bidirectional pipes / readiness handshake / timed reads and
 *        writes / terminate-and-reap (Python/Rust dual-language startup).
 */

#include "airy_memory.h"
#include "error.h"
#include "service.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if AIRY_PLATFORM_POSIX
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#include "agent_service_internal.h"

#include "airy_dirent.h"

#if AIRY_PLATFORM_POSIX

/* invoke read-response timeout (seconds). Default 300s (5 min) covers real LLM
 * calls; overridable via AIRY_AGENT_INVOKE_TIMEOUT_S. */
#define AGENT_INVOKE_TIMEOUT_S 300
/* spawn ready timeout (seconds) after starting the child. Python runner cold
 * start includes dependency imports, typically 2-5s; the default 15s tolerates
 * slow environments and treats timeout as spawn failure (P0-2). Overridable via
 * AIRY_AGENT_SPAWN_TIMEOUT_S. */
#define AGENT_SPAWN_READY_TIMEOUT_S 15

int agent_invoke_timeout_s(void)
{
    const char *env = getenv("AIRY_AGENT_INVOKE_TIMEOUT_S");
    if (env && env[0] != '\0') {
        long v = strtol(env, NULL, 10);
        if (v > 0)
            return (int)v;
    }
    return AGENT_INVOKE_TIMEOUT_S;
}

int agent_spawn_ready_timeout_s(void)
{
    const char *env = getenv("AIRY_AGENT_SPAWN_TIMEOUT_S");
    if (env && env[0] != '\0') {
        long v = strtol(env, NULL, 10);
        if (v > 0)
            return (int)v;
    }
    return AGENT_SPAWN_READY_TIMEOUT_S;
}

/* Write all bytes to fd (handles EINTR and short writes).
 * Returns 0 on success, -1 on failure (including EPIPE — child exited). */
int agent_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Read one line ('\n'-terminated) from fd with timeout, short-polling a cancel
 * token (improvement 1). On success buf holds a '\n'-less null-terminated
 * string. Returns: 0=full line; 1=buffer full/truncated (buf null-terminated,
 * caller must mark it); -1=timeout/EOF/error; -2=cancel (token hit).
 * select polls in 200ms slices to keep cancellation granularity; byte-by-byte
 * reads avoid cross-line buffering. */
int agent_read_line_timeout_ex(int fd, char *buf, size_t buf_size, int timeout_s,
                               airy_cancel_token_t *token)
{
    if (buf_size < 2)
        return -1;
    uint64_t deadline_ms = airy_time_ms() + (uint64_t)(timeout_s > 0 ? timeout_s : 300) * 1000U;
    size_t pos = 0;
    while (pos < buf_size - 1) {

        if (token && airy_cancel_token_is_canceled(token))
            return -2;
        uint64_t now = airy_time_ms();
        if (now >= deadline_ms)
            return -1;
        uint32_t remain = (uint32_t)(deadline_ms - now);
        if (remain > 200U)
            remain = 200U;

        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = remain / 1000U;
        tv.tv_usec = (remain % 1000U) * 1000U;
        int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rv == 0)
            continue;
        ssize_t n = read(fd, buf + pos, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
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

/* Extract the language field from the spec JSON, default "python".
 * Writes to buf (size bytes), always null-terminated. */
static void spec_get_language(const char *spec, char *buf, size_t size)
{
    if (!spec || size == 0) {
        if (size > 0)
            buf[0] = '\0';
        return;
    }
    cJSON *root = cJSON_Parse(spec);
    if (!root) {
        snprintf(buf, size, "python");
        return;
    }
    cJSON *lang = cJSON_GetObjectItem(root, "language");
    if (lang && cJSON_IsString(lang) && lang->valuestring[0] != '\0') {
        snprintf(buf, size, "%s", lang->valuestring);
    } else {
        snprintf(buf, size, "python");
    }
    cJSON_Delete(root);
}

/* Resolve the Rust agent binary path.
 * Priority: spec.binary_path > ${AIRY_RUST_AGENT_DIR}/<role>_agent.
 * Result written to out_path (AIRY_PATH_MAX bytes), always null-terminated. */
static void spec_resolve_rust_binary(const char *spec, const char *agent_id, char *out_path)
{
    if (!spec || !out_path) {
        if (out_path)
            out_path[0] = '\0';
        return;
    }
    cJSON *root = cJSON_Parse(spec);
    if (!root) {
        out_path[0] = '\0';
        return;
    }

    cJSON *bin = cJSON_GetObjectItem(root, "binary_path");
    if (bin && cJSON_IsString(bin) && bin->valuestring[0] != '\0') {
        snprintf(out_path, AIRY_PATH_MAX, "%s", bin->valuestring);
        cJSON_Delete(root);
        return;
    }
    /* 2. ${AIRY_RUST_AGENT_DIR}/<role>_agent */
    cJSON *role = cJSON_GetObjectItem(root, "role");
    const char *role_str = (role && cJSON_IsString(role)) ? role->valuestring : "unknown";
    const char *agent_dir = getenv("AIRY_RUST_AGENT_DIR");
    if (agent_dir && agent_dir[0] != '\0') {
        snprintf(out_path, AIRY_PATH_MAX, "%s/%s_agent", agent_dir, role_str);
    } else {
        out_path[0] = '\0';
    }
    cJSON_Delete(root);

    (void)agent_id;
}

/* ── agent 子进程日志保留策略（2026-08-25）────────────────────────
 * agent_<id>.log 随每次 spawn 创建，长期运行会无限堆积（实测数百个、
 * 数十 MB 量级）。每次 spawn 成功后清理，仅保留最近 AGENT_LOG_KEEP_MAX
 * 个（按 mtime，最旧优先删除），防止日志目录失控（AIRY_LOG_DIR 现
 * 统一收敛于 $AIRY_HOME/data/agentrt/logs）。 */
#define AGENT_LOG_KEEP_MAX 64
#define AGENT_LOG_SCAN_MAX 512

static void agent_logs_prune(void)
{
    const char *logdir = airy_log_dir();
    if (!logdir || !logdir[0])
        return;

    DIR *dir = opendir(logdir);
    if (!dir)
        return;

    char *paths[AGENT_LOG_SCAN_MAX] = {0};
    time_t mtimes[AGENT_LOG_SCAN_MAX] = {0};
    size_t count = 0;

    struct dirent *entry;
    while (count < AGENT_LOG_SCAN_MAX && (entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "agent_", 6) != 0)
            continue;
        size_t nlen = strlen(entry->d_name);
        if (nlen < 5 || strcmp(entry->d_name + nlen - 4, ".log") != 0)
            continue;

        char full[AIRY_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", logdir, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        char *copy = AIRY_STRDUP(full);
        if (!copy)
            continue;

        /* 插入排序维持 mtime 升序（最旧在前，便于批量删除）。 */
        size_t pos = count;
        while (pos > 0 && mtimes[pos - 1] > st.st_mtime) {
            paths[pos] = paths[pos - 1];
            mtimes[pos] = mtimes[pos - 1];
            pos--;
        }
        paths[pos] = copy;
        mtimes[pos] = st.st_mtime;
        count++;
    }
    closedir(dir);

    if (count <= (size_t)AGENT_LOG_KEEP_MAX) {
        for (size_t i = 0; i < count; i++)
            AIRY_FREE(paths[i]);
        return;
    }

    size_t excess = count - (size_t)AGENT_LOG_KEEP_MAX;
    for (size_t i = 0; i < excess; i++) {
        remove(paths[i]);
        AIRY_FREE(paths[i]);
    }
    for (size_t i = excess; i < count; i++)
        AIRY_FREE(paths[i]);
}

/* fork the Agent runner child process and set up bidirectional stdin/stdout
 * pipes. Startup is chosen by spec.language:
 *   - "python" (default): python3 -m airymax_agents.runner --spec <spec>
 *   - "rust": <binary_path> --spec <spec>
 * Uses execvp (no shell, no injection risk).
 * Returns 0 on success with out_pid/out_stdin/out_stdout filled; -1 on failure.
 * stderr is redirected to ${AIRY_RUNTIME_DIR}/agent_<agent_id>.log for debugging. */
int agent_spawn_child(const char *spec, const char *agent_id, pid_t *out_pid, int *out_stdin,
                      int *out_stdout)
{
    char lang[16] = {0};
    spec_get_language(spec, lang, sizeof(lang));

    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) < 0)
        return -1;
    if (pipe(stdout_pipe) < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }

    if (pid == 0) {

        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        /* Improvement 1: create own process group (setpgid(0,0)) so termination
         * cascades SIGTERM/SIGKILL to all processes spawned by the runner
         * (graceful kill of the whole process tree). */
        setpgid(0, 0);
        if (dup2(stdin_pipe[0], STDIN_FILENO) < 0)
            _exit(127);
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        /* stderr -> log file (best-effort; on failure inherit parent stderr).
         * Logs are collected under AIRY_HOME/logs (run/ holds only sockets
         * and pid files; agent logs belong with the other daemon logs). */
        char log_path[AIRY_PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/agent_%s.log", airy_log_dir(), agent_id);
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        if (strcmp(lang, "rust") == 0) {

            char bin_path[AIRY_PATH_MAX] = {0};
            spec_resolve_rust_binary(spec, agent_id, bin_path);
            if (bin_path[0] == '\0') {

                SVC_LOG_WARN("Rust agent binary path empty, fallback to python3: agent_id=%s",
                             agent_id);
                goto fallback_python;
            }
            char *argv[] = {
                bin_path,
                (char *)"--spec",
                (char *)spec,
                NULL,
            };
            execvp(bin_path, argv);

            SVC_LOG_WARN("execvp Rust agent failed, fallback to python3: binary=%s, agent_id=%s",
                         bin_path, agent_id);
        }

    fallback_python:
        /* Python runner (default & fallback path)
         *
         * Dependency resolution: airymax_agents / openlab / agentrt SDK are
         * resolved through standard Python package installation (pip install
         * -e, see ecosystem three-package packaging); no more PYTHONPATH
         * injection or source-tree inference from the executable location
         * (removal of the historical P0-1 mechanism, see
         * docs-closed/agentrt/01-designs/_design_0.1.1/06-agent-gateway-wiring.md §3.1). */
        {
            char *argv[] = {
                (char *)"python3", (char *)"-m", (char *)"airymax_agents.runner",
                (char *)"--spec",  (char *)spec, NULL,
            };
            execvp("python3", argv);
        }

        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    *out_pid = pid;
    *out_stdin = stdin_pipe[1];
    *out_stdout = stdout_pipe[0];

    /* 2026-08-25：spawn 成功后收敛 agent 子进程日志数量（保留最近 64 个）。 */
    agent_logs_prune();
    return 0;
}

/* Reap the child process and close the pipe handles (terminate / invoke
 * failure / destroy calls). First SIGTERM the process group, wait up to 2
 * seconds, SIGKILL the group if it still does not exit, then waitpid reaps the
 * group-leader zombie (the child called setpgid(0,0) at spawn, so the negative
 * pid signal cascades to all processes spawned by the runner). */
void agent_kill_and_reap(pid_t *pid_ptr, int *stdin_ptr, int *stdout_ptr)
{
    pid_t pid = *pid_ptr;
    if (pid <= 0)
        return;

    kill(-pid, SIGTERM);

    for (int i = 0; i < 20; i++) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0)
            break;

        struct timespec ts = {0, 100 * 1000000L};
        nanosleep(&ts, NULL);
    }

    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(-pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    if (*stdin_ptr >= 0) {
        close(*stdin_ptr);
        *stdin_ptr = -1;
    }
    if (*stdout_ptr >= 0) {
        close(*stdout_ptr);
        *stdout_ptr = -1;
    }
    *pid_ptr = -1;
}
#endif /* AIRY_PLATFORM_POSIX */
