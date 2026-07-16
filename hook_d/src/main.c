// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file main.c
 * @brief Hook 守护进程入口（P0.18.1 样板宏化）
 * @owner team-A
 */

#include "hook_service.h"
#include "airy_memory.h"
#include "error.h"
#include "daemon_main.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#define HOOK_D_SOCKET_PATH AIRY_RUNTIME_DIR "/hook.sock"
#define HOOK_D_PIPE_PATH   "\\\\.\\pipe\\airy_hook"

/* P0.18.1: 使用 DAEMON_DECLARE_COMMON 生成公共样板（信号处理/全局变量/print_usage） */
DAEMON_DECLARE_COMMON(hook_d, hook, HOOK_D_SOCKET_PATH, HOOK_D_PIPE_PATH, 0, 4096)

/* 销毁服务（daemon_cleanup_standard 回调） */
static void destroy_service_hook_d(void)
{
    daemon_cupolas_cleanup();
}

#ifdef _WIN32
/* Windows 控制台事件处理（复用生成的 signal_handler_hook_d） */
static BOOL WINAPI console_handler_hook_d(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_hook_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_hook_d);

    /* 跨平台信号处理 */
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler_hook_d, TRUE);
#else
    DAEMON_SETUP_SIGNALS(hook_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    /* P3.14 ACC-DT15: 初始化 cupolas 安全穹顶 */
    daemon_cupolas_init("hook_d");
    SVC_LOG_INFO("hook_d: starting");

    /* 创建 Socket 服务器 */
    airy_sock_t server_fd =
        daemon_create_server_socket(0, 0, HOOK_D_SOCKET_PATH, HOOK_D_PIPE_PATH);
    if (server_fd < 0) {
        SVC_LOG_ERROR("P2.1: HookD: failed to create socket at %s (errno=%d: %s)",
                      HOOK_D_SOCKET_PATH, errno, strerror(errno));
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO("P2.1: HookD: listening on %s (fd=%d)", HOOK_D_SOCKET_PATH, (int)server_fd);

    g_bsd_hook_d = daemon_bootstrap_sd_start("hook_d", "hook", HOOK_D_SOCKET_PATH,
                                              0, "hook,core", 0);
    g_bipc_hook_d = daemon_bootstrap_ipc_start("hook_d", "hook", HOOK_D_SOCKET_PATH,
                                               0, IPC_BUS_PROTO_JSON_RPC);
    if (!g_bsd_hook_d)
        SVC_LOG_WARN("P2.1: HookD: SD bootstrap failed, continuing");
    if (!g_bipc_hook_d)
        SVC_LOG_WARN("P2.1: HookD: IPC bootstrap failed, continuing");

    SVC_LOG_INFO("P2.1: HookD: running (sd=%s ipc=%s), waiting for shutdown signal",
                 g_bsd_hook_d ? "ok" : "no", g_bipc_hook_d ? "ok" : "no");

    /* 等待关闭信号 */
    while (atomic_load_explicit(&g_running_hook_d, memory_order_acquire)) {
        sleep(1);
    }

    SVC_LOG_INFO("P2.1: HookD: shutting down (sd=%s ipc=%s)",
                 g_bsd_hook_d ? "stopped" : "n/a", g_bipc_hook_d ? "stopped" : "n/a");
    daemon_cleanup_standard(g_bipc_hook_d, g_bsd_hook_d, NULL, server_fd,
                            destroy_service_hook_d, &g_running_lock_hook_d);
    log_cleanup();
    return EXIT_SUCCESS;
}
