// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file service_child_posix.c
 * @brief POSIX agent runner child transport: fork / bidirectional pipes /
 *        timed reads / group terminate-and-reap. Windows counterpart:
 *        service_child_win.c (0.1.21 P0-1).
 */

#include "io.h"

#include "agent_child_internal.h"

#if AIRY_PLATFORM_POSIX

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

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
        int st = agent_line_step(buf, buf_size, &pos, n == 0);
        if (st != AGENT_LINE_MORE)
            return st;
    }
    return 1;
}

/* 0.1.21 D1：exec 前关闭 3 号以上的全部描述符。守护的 RPC 连接/监听 fd
 * 已置 FD_CLOEXEC，这里再兜底扫一遍，确保任何来源的 fd 都不会泄漏给 agent
 * 子进程——否则守护关闭本端后对端永远等不到 EOF，以 EOF 判完成的客户端
 * （gateway gw_svc_call）会在 agent.spawn/invoke 上挂到子进程退出。
 * 0/1/2 此时已分别 dup2 为子进程的 stdin/stdout/stderr。 */
static void agent_close_fds(void)
{
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536)
        max_fd = 65536;
    for (int fd = 3; fd < (int)max_fd; fd++)
        (void)close(fd);
}

/* fork the Agent runner child process and set up bidirectional stdin/stdout
 * pipes. Startup is chosen by spec.language:
 *   - "python" (default): python3 -m airymax_agents.runner --spec <spec>
 *   - "rust": <binary_path> --spec <spec>
 * Uses execvp (no shell, no injection risk).
 * Returns 0 on success with out_pid/out_stdin/out_stdout filled; -1 on failure.
 * stderr is redirected to <airy_log_dir>/agent_<agent_id>.log for debugging. */
int agent_spawn_child(const char *spec, const char *agent_id, long *out_pid, int *out_stdin,
                      int *out_stdout)
{
    char lang[16] = {0};
    agent_spec_language(spec, lang, sizeof(lang));

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
        agent_child_log_path(agent_id, log_path, sizeof(log_path));
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        /* 0.1.21 D1：此后不再需要任何来自守护的描述符。 */
        agent_close_fds();

        if (strcmp(lang, "rust") == 0) {

            char bin_path[AIRY_PATH_MAX] = {0};
            agent_spec_rust_bin(spec, bin_path, sizeof(bin_path));
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
         * 06-agent-gateway-wiring.md §3.1). */
        {
            /* 0.1.14 修复：剥离运行时 lib 目录的 LD_LIBRARY_PATH 污染。
             * agent_d 进程环境的 LD_LIBRARY_PATH 含 $AIRY_LIB_DIR（自包含
             * 打包的 agentrt 私有 .so），子进程 python3 加载 _sqlite3 等
             * 系统扩展时会优先解析到打包的旧版 libsqlite3.so.0（缺
             * sqlite3_deserialize 符号）→ import 崩溃 → readiness 握手
             * EOF → agent.spawn 全部失败 → DAG 任务节点执行必败
             * （0.1.13 实机回归实锤）。Python runner 为纯 Python 自包含，
             * 无需 runtime lib；系统库走默认加载路径即可。 */
            unsetenv("LD_LIBRARY_PATH");
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
    *out_pid = (long)pid;
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
void agent_kill_and_reap(long *pid_ptr, int *stdin_ptr, int *stdout_ptr)
{
    pid_t pid = (pid_t)*pid_ptr;
    if (pid <= 0)
        return;

    kill(-pid, SIGTERM);

    for (int i = 0; i < 20; i++) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || r < 0)
            break;

        airy_sleep_ms(100);
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

#else /* !AIRY_PLATFORM_POSIX */

/* Avoid an empty translation unit under -Wpedantic on Windows. */
typedef int agent_child_posix_unused_t;

#endif /* AIRY_PLATFORM_POSIX */
