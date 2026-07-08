// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file daemon_main.h
 * @brief Daemon 主函数样板宏与公共辅助函数
 *
 * P0.18.1: 消除 12 个 daemon main.c 的重复样板代码（约 5,956 → 约 1,500 行）。
 *
 * 架构设计（P1.23 + daemon_startup.h）：
 * - 12 daemon 的主函数结构本质相同：init → create → run → cleanup
 * - 差异点仅在于：服务创建函数、方法注册表、服务销毁函数
 * - 本文件将完全相同的样板提取为 inline 函数，差异点通过模板宏注入
 *
 * 使用方法：
 *   在每个 daemon 的 main.c 中，只需要：
 *   1. 定义服务全局变量
 *   2. 定义方法处理器
 *   3. 调用 DAEMON_MAIN_BOILERPLATE() 生成主函数
 *
 * @see ARCHITECTURAL_PRINCIPLES.md E-3~E-6
 */

#ifndef AGENTRT_DAEMON_MAIN_H
#define AGENTRT_DAEMON_MAIN_H

#include "daemon_bootstrap_ipc.h"
#include "daemon_bootstrap_sd.h"
#include "daemon_cupolas_bootstrap.h"
#include "daemon_event_driver.h"
#include "daemon_platform_ext.h"
#include "jsonrpc_helpers.h"
#include "logging.h"
#include "method_dispatcher.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>
/* P0.18.2: 引入 cjson_helpers.h 提供 CJSON_PARSE_GUARD/CJSON_AUTO_FREE 宏 */
#include <cjson_helpers.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 全局公共声明 ==================== */

/**
 * @brief 生成 daemon main.c 的公共全局变量和信号处理声明
 *
 * @param daemon_name  daemon 进程名（如 "sched_d"）
 * @param daemon_cname daemon 配置和服务名（如 "scheduler"）
 *
 * 生成变量：
 *   - static atomic_int g_running = 1
 *   - static agentrt_mutex_t g_running_lock
 *   - static method_dispatcher_t *g_dispatcher = NULL
 *   - static daemon_event_driver_t *g_event_driver = NULL
 *   - static daemon_bootstrap_sd_t *g_bsd = NULL
 *   - static daemon_bootstrap_ipc_t *g_bipc = NULL
 *
 * 生成函数：
 *   - static void signal_handler(int sig)
 *   - static void svc_log_toggle_handler(int sig)
 *   - static void print_usage(const char *prog, const char *service_name)
 *   - static int daemon_handle_client(daemon_event_driver_t *driver,
 *         agentrt_socket_t client_fd, method_dispatcher_t *dispatcher)
 */
#define DAEMON_DECLARE_COMMON(daemon_name, daemon_cname, DEFAULT_SOCKET_PATH_UNIX,   \
                              DEFAULT_SOCKET_PATH_WIN, DEFAULT_TCP_PORT, MAX_BUFFER) \
                                                                                     \
    static atomic_int g_running_##daemon_name = 1;                                   \
    static agentrt_mutex_t g_running_lock_##daemon_name;                              \
    static method_dispatcher_t *g_dispatcher_##daemon_name = NULL;                    \
    static daemon_event_driver_t *g_event_driver_##daemon_name = NULL;                \
    static daemon_bootstrap_sd_t *g_bsd_##daemon_name = NULL;                         \
    static daemon_bootstrap_ipc_t *g_bipc_##daemon_name = NULL;                       \
                                                                                     \
    static void signal_handler_##daemon_name(int sig)                                 \
    {                                                                                \
        (void)sig;                                                                   \
        agentrt_mutex_lock(&g_running_lock_##daemon_name);                            \
        atomic_store_explicit(&g_running_##daemon_name, 0, memory_order_seq_cst);     \
        agentrt_mutex_unlock(&g_running_lock_##daemon_name);                          \
        if (g_event_driver_##daemon_name)                                             \
            daemon_event_driver_stop(g_event_driver_##daemon_name);                   \
    }                                                                                \
                                                                                     \
    static void svc_log_toggle_handler_##daemon_name(int sig)                         \
    {                                                                                \
        (void)sig;                                                                   \
        static int debug_mode = 0;                                                   \
        debug_mode = !debug_mode;                                                    \
        log_set_module_level("*", debug_mode ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);    \
    }                                                                                \
                                                                                     \
    static void print_usage_##daemon_name(const char *prog)                           \
    {                                                                                \
        char buf[256];                                                               \
        fputs("AgentRT " #daemon_name " (" #daemon_cname ")\n", stdout);              \
        snprintf(buf, sizeof(buf), "Usage: %s [options]\n\n", prog);                 \
        fputs(buf, stdout);                                                          \
        fputs("Options:\n", stdout);                                                 \
        fputs("  --manager <path>   Configuration file path\n", stdout);             \
        fputs("  --tcp             Use TCP instead of Unix socket\n", stdout);       \
        fputs("  --help             Show this help\n", stdout);                      \
        fputs("\n", stdout);                                                         \
        fputs("Examples:\n", stdout);                                                \
        snprintf(buf, sizeof(buf), "  %s --manager "                                  \
                 AGENTRT_CONFIG_DIR " \"/" #daemon_name ".yaml\"\n", prog);            \
        fputs(buf, stdout);                                                          \
        snprintf(buf, sizeof(buf), "  %s --tcp  # TCP mode on port %d\n",             \
                 prog, DEFAULT_TCP_PORT);                                             \
        fputs(buf, stdout);                                                          \
    }                                                                                \
                                                                                     \
    static int daemon_handle_client_##daemon_name(                                    \
        agentrt_socket_t client_fd, method_dispatcher_t *dispatcher)                  \
    {                                                                                \
        char buffer[MAX_BUFFER];                                                     \
        ssize_t n = agentrt_socket_recv(client_fd, buffer, sizeof(buffer) - 1);      \
        if (n <= 0) {                                                                \
            agentrt_socket_close(client_fd);                                         \
            return -1;                                                               \
        }                                                                            \
        buffer[n] = '\0';                                                            \
        if ((size_t)n >= sizeof(buffer) - 1) {                                       \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST,                    \
                               "Request too large", -1);                              \
            agentrt_socket_close(client_fd);                                         \
            return -1;                                                               \
        }                                                                            \
        /* P0.18.2: 模式 A — CJSON_PARSE_GUARD 自动释放 + NULL 检查 */                \
        CJSON_PARSE_GUARD(req, buffer, {                                             \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_PARSE_ERROR,                        \
                               "Parse error: invalid JSON", -1);                      \
            agentrt_socket_close(client_fd);                                         \
            return -1;                                                               \
        });                                                                          \
        cJSON *jsonrpc = cJSON_GetObjectItem(req, "jsonrpc");                        \
        cJSON *method = cJSON_GetObjectItem(req, "method");                          \
        cJSON *id = cJSON_GetObjectItem(req, "id");                                  \
        if (!cJSON_IsString(jsonrpc) ||                                              \
            strcmp(jsonrpc->valuestring, "2.0") != 0 ||                              \
            !cJSON_IsString(method) || !id) {                                        \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST,                    \
                               "Invalid Request", -1);                               \
            /* req 由 CJSON_AUTO_FREE 自动释放 */                                    \
            agentrt_socket_close(client_fd);                                         \
            return -1;                                                               \
        }                                                                            \
        int req_id = cJSON_IsNumber(id) ? id->valueint : 0;                          \
        SVC_LOG_DEBUG("Processing request: method=%s, id=%d",                        \
                      method->valuestring, req_id);                                   \
        method_dispatcher_dispatch(dispatcher, req, jsonrpc_build_error,              \
                                   &client_fd);                                      \
        /* req 由 CJSON_AUTO_FREE 自动释放 */                                        \
        agentrt_socket_close(client_fd);                                             \
        return 0;                                                                    \
    }                                                                                \
                                                                                     \
    static int daemon_on_client_##daemon_name(                                        \
        void *service_ctx, agentrt_socket_t client_fd)                                \
    {                                                                                \
        (void)service_ctx;                                                           \
        return daemon_handle_client_##daemon_name(client_fd,                           \
                           g_dispatcher_##daemon_name);                               \
    }


/* ==================== 公共初始化辅助函数 ==================== */

/**
 * @brief 跨平台信号处理设置
 *
 * @param daemon_name 用于生成独特的信号处理函数名称
 *
 * 在所有 daemon 中相同：
 * - POSIX: SIGINT/SIGTERM → signal_handler, SIGPIPE → IGN, SIGUSR1 → svc_log_toggle
 * - Windows: SetConsoleCtrlHandler → console_handler
 */
#define DAEMON_SETUP_SIGNALS(daemon_name)                                              \
    do {                                                                               \
        signal(SIGINT, signal_handler_##daemon_name);                                   \
        signal(SIGTERM, signal_handler_##daemon_name);                                  \
        signal(SIGPIPE, SIG_IGN);                                                      \
        signal(SIGUSR1, svc_log_toggle_handler_##daemon_name);                          \
    } while (0)


/**
 * @brief 命令行参数解析（--manager, --tcp, --help）
 *
 * @param config_path 配置路径指针，会被修改
 * @param use_tcp     TCP 标记指针，会被修改
 *
 * 返回值：0=继续, >0=exit(code), <0=error
 */
static inline int daemon_parse_args(int argc, char **argv,
                                    const char **config_path,
                                    int *use_tcp,
                                    void (*print_usage_fn)(const char *))
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--manager") == 0 && i + 1 < argc) {
            *config_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            if (print_usage_fn) print_usage_fn(argv[0]);
            return 1; /* exit(0) */
        } else if (strcmp(argv[i], "--tcp") == 0) {
            *use_tcp = 1;
        } else {
            SVC_LOG_ERROR("Unknown option: %s", argv[i]);
            if (print_usage_fn) print_usage_fn(argv[0]);
            return 2; /* exit(1) */
        }
    }
    return 0;
}


/**
 * @brief 创建服务器 Socket（TCP 或 Unix）
 *
 * 统一封装 TCP/Unix Socket 创建逻辑，消除各 daemon 的重复分支。
 *
 * @param use_tcp     是否使用 TCP
 * @param tcp_port    TCP 端口号（use_tcp=true 时使用）
 * @param unix_path   Unix Socket 路径（use_tcp=false 时使用）
 * @param win_pipe    Windows Named Pipe 路径
 * @return            成功返回 socket fd，失败返回 < 0
 */
static inline agentrt_socket_t daemon_create_server_socket(
    int use_tcp, int tcp_port,
    const char *unix_path, const char *win_pipe)
{
    if (use_tcp) {
        return agentrt_socket_create_tcp_server("127.0.0.1", tcp_port);
    }
#if defined(AGENTRT_PLATFORM_WINDOWS)
    (void)unix_path;
    return agentrt_socket_create_named_pipe_server(win_pipe);
#else
    (void)win_pipe;
    return agentrt_socket_create_unix_server(unix_path);
#endif
}


/**
 * @brief 创建并启动 Event Driver 与 Bootstrap 服务
 *
 * @param daemon_name  daemon 名（如 "sched_d"）
 * @param service_type 服务发现类型（如 "scheduler"）
 * @param socket_path  Socket 路径（Unix）或 "127.0.0.1"（TCP）
 * @param tcp_port     TCP 端口（0=Unix）
 * @param tags         服务标签（如 "scheduler,core"）
 * @param use_tcp      是否 TCP 模式
 * @param ev_config    事件驱动配置
 * @param p_event_driver 输出：事件驱动实例
 * @param p_bsd        输出：SD bootstrap 实例
 * @param p_bipc       输出：IPC bootstrap 实例
 * @return             AGENTRT_SUCCESS 或错误码
 */
static inline int daemon_init_event_driver(
    const char *daemon_name,
    const char *service_type,
    const char *socket_path,
    int tcp_port,
    const char *tags,
    int use_tcp,
    const daemon_event_config_t *ev_config,
    daemon_event_driver_t **p_event_driver,
    daemon_bootstrap_sd_t **p_bsd,
    daemon_bootstrap_ipc_t **p_bipc)
{
    /* 服务发现 bootstrap */
    if (p_bsd) {
        const char *sd_addr = use_tcp ? "127.0.0.1" : socket_path;
        *p_bsd = daemon_bootstrap_sd_start(daemon_name, service_type,
                                            sd_addr, tcp_port, tags, 0);
    }

    /* IPC bootstrap */
    if (p_bipc) {
        const char *ipc_addr = use_tcp ? "127.0.0.1" : socket_path;
        *p_bipc = daemon_bootstrap_ipc_start(daemon_name, service_type,
                                              ipc_addr, tcp_port,
                                              IPC_BUS_PROTO_JSON_RPC);
    }

    /* 事件驱动 */
    if (!ev_config || !p_event_driver) return AGENTRT_ERR_INVALID_PARAM;
    *p_event_driver = daemon_event_driver_create(ev_config);
    if (!*p_event_driver) return AGENTRT_ERR_OUT_OF_MEMORY;

    return AGENTRT_SUCCESS;
}


/**
 * @brief 标准 daemon 资源清理链
 *
 * 按照与 init 相反的顺序清理所有资源：
 *   bootstrap_ipc → bootstrap_sd → event_driver → socket → service → mutex → socket_cleanup → cupolas → log
 */
static inline void daemon_cleanup_standard(
    daemon_bootstrap_ipc_t *bipc,
    daemon_bootstrap_sd_t *bsd,
    daemon_event_driver_t *event_driver,
    agentrt_socket_t server_fd,
    void (*destroy_service)(void),
    agentrt_mutex_t *running_lock)
{
    SVC_LOG_INFO("Service stopping...");

    if (bipc)  daemon_bootstrap_ipc_stop(bipc);
    if (bsd)   daemon_bootstrap_sd_stop(bsd);
    if (event_driver) daemon_event_driver_destroy(event_driver);
    if (server_fd >= 0) agentrt_socket_close(server_fd);
    if (destroy_service) destroy_service();
    if (running_lock) agentrt_mutex_destroy(running_lock);
    agentrt_socket_cleanup();

    SVC_LOG_INFO("Service stopped");
}


#ifdef __cplusplus
}
#endif

#endif /* AGENTRT_DAEMON_MAIN_H */
