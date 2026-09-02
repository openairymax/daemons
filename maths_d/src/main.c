// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file main.c
 * @brief 数学外挂计算 daemon 主入口（maths.* 命名空间）。
 *
 * 定位：Agent 运行时数学计算外挂（Tri-Opt 3.3 "外挂计算器" 的工程实现）。
 * 纯本地 C 求值，把数学表达式从 LLM 推理中剥离（约 5 token 取代
 * 150~300 token）。仅监听 Unix Socket（maths.sock），JSON-RPC 方法：
 *   eval / stats / recognize / health_check / get_stats / shutdown
 *
 * Conventions: daemon_main.h（E-3/E-4/E-5/E-6）、svc_logger、airy_common。
 */

#include "airy_memory.h"
#include "daemon_main.h"
#include "maths_service.h"
#include "platform.h"
#include "svc_common.h"
#include "svc_logger.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#endif

#define MATHS_DEFAULT_SOCKET airy_runtime_dir_socket("maths.sock")
#define MATHS_DEFAULT_PORT 8087

static maths_d_service_t g_service;
static atomic_int g_shutdown = 0;
static daemon_bootstrap_sd_t *g_bsd = NULL;
static daemon_bootstrap_ipc_t *g_bipc = NULL;

static void maths_d_signal_handler(int sig)
{
    (void)sig;
    atomic_store_explicit(&g_shutdown, 1, memory_order_seq_cst);
#ifndef _WIN32
    static const char sig_msg[] =
        "[SIG] shutdown signal received, initiating graceful shutdown\n";
    (void)write(STDERR_FILENO, sig_msg, sizeof(sig_msg) - 1);
#endif
}

/* 连接处理：单请求短连接（JSON-RPC over unix socket），recv 前 poll 等待
 * 首包最多 3s，防慢连接阻塞 accept 循环。 */
static void maths_d_handle_request(maths_d_service_t *svc, airy_sock_t client_fd)
{
    char buffer[8192];
#ifndef _WIN32
    struct pollfd pfd;
    pfd.fd = (int)client_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, 3000);
    if (pr <= 0 || !(pfd.revents & POLLIN)) {
        airy_sock_close(client_fd);
        return;
    }
#endif
    ssize_t n = airy_sock_recv(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        airy_sock_close(client_fd);
        return;
    }
    buffer[n] = '\0';

    char response[8192];
    int dispatch_rc =
        maths_d_dispatch_jsonrpc(svc, buffer, response, sizeof(response));
    if (dispatch_rc == MATHS_METHOD_SHUTDOWN) {
        atomic_store_explicit(&g_shutdown, 1, memory_order_seq_cst);
    }
    if (dispatch_rc == MATHS_METHOD_HANDLED ||
        dispatch_rc == MATHS_METHOD_SHUTDOWN) {
        airy_sock_send(client_fd, response, strlen(response));
    }
    airy_sock_close(client_fd);
}

static int maths_d_init(maths_d_service_t *svc, const char *sock, int port)
{
    if (maths_d_service_init(svc) != 0)
        return -1;
    svc->socket_path =
        sock && sock[0] ? AIRY_STRDUP(sock) : AIRY_STRDUP(MATHS_DEFAULT_SOCKET);
    if (!svc->socket_path)
        return -1;
    svc->tcp_port = port > 0 ? port : MATHS_DEFAULT_PORT;
    airy_sock_init();
    /* 拉起 Python 符号后端（maths-toolkit market 包）；不可用则纯 C 降级 */
    maths_backend_init(&svc->py_backend, NULL);
    SVC_LOG_INFO("maths_d: init complete (socket=%s, python_backend=%s)",
                 svc->socket_path,
                 maths_backend_available(&svc->py_backend) ? "up" : "degraded");
    return 0;
}

static int maths_d_start(maths_d_service_t *svc)
{
#ifndef _WIN32
    svc->server_fd = airy_sock_create_unix_server(svc->socket_path);
    if (svc->server_fd < 0) {
        SVC_LOG_ERROR("maths_d: failed to create socket at %s", svc->socket_path);
        return -1;
    }
#else
    svc->server_fd = airy_sock_create_tcp_server("127.0.0.1",
                                                 (uint16_t)svc->tcp_port);
    if (svc->server_fd < 0) {
        SVC_LOG_ERROR("maths_d: failed to create TCP server");
        return -1;
    }
#endif
    atomic_store(&svc->running, 1);
    SVC_LOG_INFO("maths_d: service started");
    return 0;
}

static void maths_d_stop(maths_d_service_t *svc, int force)
{
    atomic_store(&svc->running, 0);
    if (svc->server_fd != AIRY_INVALID_SOCKET) {
        airy_sock_close(svc->server_fd);
        svc->server_fd = AIRY_INVALID_SOCKET;
    }
    if (force) {
#ifndef _WIN32
        if (svc->socket_path)
            unlink(svc->socket_path);
#endif
    }
    SVC_LOG_INFO("maths_d: service stopped");
}

static void maths_d_destroy(maths_d_service_t *svc)
{
    maths_d_stop(svc, 1);
    maths_backend_destroy(&svc->py_backend);
    maths_d_service_destroy(svc);
    airy_sock_cleanup();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#ifndef _WIN32
    signal(SIGINT, maths_d_signal_handler);
    signal(SIGTERM, maths_d_signal_handler);
    signal(SIGPIPE, SIG_IGN);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    daemon_cupolas_init_pep("maths_d");

    if (maths_d_init(&g_service, MATHS_DEFAULT_SOCKET, MATHS_DEFAULT_PORT) != 0)
        return EXIT_FAILURE;
    if (maths_d_start(&g_service) != 0) {
        maths_d_destroy(&g_service);
        return EXIT_FAILURE;
    }

    g_bsd = daemon_bootstrap_sd_start("maths_d", "maths", g_service.socket_path,
                                      0, "maths,core", 0);
    g_bipc = daemon_bootstrap_ipc_start("maths_d", "maths", g_service.socket_path,
                                        0, IPC_BUS_PROTO_JSON_RPC);

    while (!g_shutdown && atomic_load(&g_service.running)) {
        airy_sock_t client = airy_sock_accept(g_service.server_fd, 1000);
        if (client != AIRY_INVALID_SOCKET)
            maths_d_handle_request(&g_service, client);
    }

    daemon_bootstrap_ipc_stop(g_bipc);
    daemon_bootstrap_sd_stop(g_bsd);
    maths_d_destroy(&g_service);
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
