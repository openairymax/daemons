// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file python_backend.c
 * @brief Python 数学后端管理实现（stdio JSON-RPC over fork/pipe）。
 *
 * 生命周期：maths_d 启动时 spawn 常驻 worker（maths_backend.py），
 * 每行一条 JSON 请求、每行一条 JSON 响应。worker 异常退出（EPIPE/EOF）
 * 时在 MATHS_BACKEND_RESET_MAX 次数内自动重启，超限降级（available=0），
 * 调用方回退纯 C 快速路径。
 */

#include "python_backend.h"

#include "airy_memory.h"
#include "platform.h"
#include "svc_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

/* 平台.h 是否提供 airy_pid_t？POSIX 下用 pid_t。 */
#include <sys/types.h>

#ifndef _WIN32

/* 默认 AIRY_HOME（与 platform.h AIRY_HOME 一致） */
static const char *backend_default_home(void)
{
    const char *home = getenv("AIRY_HOME");
    if (home && home[0])
        return home;
    return ".airymaxrt";
}

/* 构建 backend/python 候选路径；返回可用的或空串 */
static void backend_locate(const char *airy_home, char *python_out,
                           size_t python_sz, char *backend_out,
                           size_t backend_sz)
{
    const char *home = (airy_home && airy_home[0]) ? airy_home
                                                   : backend_default_home();
    char buf[1024];

    python_out[0] = '\0';
    backend_out[0] = '\0';

    /* 优先共享 venv 解释器 */
    snprintf(buf, sizeof(buf), "%s/venv/bin/python3", home);
    if (access(buf, X_OK) == 0) {
        snprintf(python_out, python_sz, "%s", buf);
    } else {
        /* 回退 PATH 中的 python3 */
        snprintf(python_out, python_sz, "python3");
    }

    snprintf(buf, sizeof(buf), "%s/backend/maths_backend.py", home);
    if (access(buf, R_OK) == 0)
        snprintf(backend_out, backend_sz, "%s", buf);
}

/* 拉起 worker 子进程（in_fd 写、out_fd 读）。返回 0 成功。 */
static int backend_spawn(maths_py_backend_t *be)
{
    int in_pipe[2];
    int out_pipe[2];

    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        SVC_LOG_ERROR("maths backend: pipe() failed (%s)", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        SVC_LOG_ERROR("maths backend: fork() failed (%s)", strerror(errno));
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* 子进程：stdin=in_pipe[0]，stdout=out_pipe[1]，stderr=/dev/null */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);

        char *argv[3];
        argv[0] = be->python_path;
        argv[1] = be->backend_path;
        argv[2] = NULL;
        execvp(be->python_path, argv);
        _exit(127);
    }

    /* 父进程 */
    close(in_pipe[0]);
    close(out_pipe[1]);
    be->in_fd = in_pipe[1];
    be->out_fd = out_pipe[0];
    be->available = 1;
    be->respawn_count = 0;
    SVC_LOG_INFO("maths backend: worker spawned (pid=%d, py=%s)",
                 (int)pid, be->python_path);
    return 0;
}

/* 终止 worker 并回收 fd */
static void backend_terminate(maths_py_backend_t *be)
{
    if (be->in_fd >= 0) {
        close(be->in_fd);
        be->in_fd = -1;
    }
    if (be->out_fd >= 0) {
        close(be->out_fd);
        be->out_fd = -1;
    }
    be->available = 0;
}

/* 读取一行响应（直到 '\n' 或超时）。返回行长度；<0 失败/超时。 */
static int backend_read_line(maths_py_backend_t *be, char *buf, size_t buf_sz)
{
    struct pollfd pfd;
    pfd.fd = be->out_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pr = poll(&pfd, 1, MATHS_BACKEND_TIMEOUT_MS);
    if (pr <= 0 || !(pfd.revents & POLLIN))
        return -1;

    size_t n = 0;
    while (n + 1 < buf_sz) {
        ssize_t r = read(be->out_fd, buf + n, 1);
        if (r == 1) {
            if (buf[n] == '\n') {
                buf[n] = '\0';
                return (int)n;
            }
            n++;
        } else {
            if (r < 0 && errno == EINTR)
                continue;
            if (n == 0)
                return -1;
            break;
        }
        if (n + 1 >= buf_sz)
            break;
    }
    buf[n] = '\0';
    return (int)n;
}

#endif /* _WIN32 */

int maths_backend_init(maths_py_backend_t *be, const char *airy_home)
{
    if (!be)
        return -1;

    AIRY_MEMSET(be, 0, sizeof(*be));
    be->in_fd = -1;
    be->out_fd = -1;

#ifndef _WIN32
    char python[512];
    char backend[512];
    backend_locate(airy_home, python, sizeof(python), backend, sizeof(backend));

    if (!backend[0]) {
        SVC_LOG_INFO("maths backend: maths_backend.py 未部署（跳过，纯 C 快速路径可用）");
        return -1;
    }
    snprintf(be->python_path, sizeof(be->python_path), "%s", python);
    snprintf(be->backend_path, sizeof(be->backend_path), "%s", backend);
    return backend_spawn(be);
#else
    (void)airy_home;
    return -1;
#endif
}

int maths_backend_call(maths_py_backend_t *be, const char *method,
                       const char *params_json, char *resp, size_t resp_sz)
{
    if (!be || !method || !resp || resp_sz == 0)
        return -1;
    if (!be->available)
        return -1;

#ifndef _WIN32
    char req[8192];
    snprintf(req, sizeof(req),
             "{\"id\":1,\"method\":\"%s\",\"params\":%s}\n", method,
             params_json ? params_json : "{}");

    if (write(be->in_fd, req, strlen(req)) != (ssize_t)strlen(req)) {
        /* worker 已死：限次重启后降级 */
        if (be->respawn_count < MATHS_BACKEND_RESET_MAX) {
            SVC_LOG_WARN("maths backend: worker write failed, respawning "
                         "(attempt %d/%d)", be->respawn_count + 1,
                         MATHS_BACKEND_RESET_MAX);
            backend_terminate(be);
            be->respawn_count++;
            if (backend_spawn(be) == 0)
                return maths_backend_call(be, method, params_json, resp,
                                          resp_sz);
        } else {
            backend_terminate(be);
            SVC_LOG_ERROR("maths backend: worker 多次异常，降级纯 C 快速路径");
        }
        return -1;
    }

    int len = backend_read_line(be, resp, resp_sz);
    if (len < 0) {
        SVC_LOG_WARN("maths backend: response timeout/EOF for %s", method);
        backend_terminate(be);
        return -1;
    }
    return 0;
#else
    (void)params_json;
    return -1;
#endif
}

int maths_backend_available(const maths_py_backend_t *be)
{
    return (be && be->available);
}

void maths_backend_destroy(maths_py_backend_t *be)
{
    if (!be)
        return;
#ifndef _WIN32
    backend_terminate(be);
#endif
}
