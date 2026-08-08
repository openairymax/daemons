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

#ifndef AIRY_RT_DAEMON_MAIN_H
#define AIRY_RT_DAEMON_MAIN_H

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

#if AIRY_PLATFORM_POSIX
#include <poll.h>
#include <unistd.h>
#endif

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
 *   - static airy_mtx_t g_running_lock
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
 *         airy_sock_t client_fd, method_dispatcher_t *dispatcher)
 */
/* SIGTERM 接收埋点：write 直接写 stderr（async-signal-safe，绕过日志锁）。
 * 必须在宏外做平台条件分支——预处理器指令（#if/#endif）不能出现在宏体内，
 * 否则 '#' 会被当作字符串化运算符导致 "'#' is not followed by a macro parameter"。 */
#if AIRY_PLATFORM_POSIX
#define DAEMON_SIG_RECEIVED_TRACE(daemon_name)                                       \
    do {                                                                             \
        static const char _sig_msg_##daemon_name[] =                                 \
            "[SIG] shutdown signal received, initiating graceful shutdown\n";         \
        if (write(STDERR_FILENO, _sig_msg_##daemon_name,                             \
                  sizeof(_sig_msg_##daemon_name) - 1) < 0) {                         \
            /* 忽略：stderr 不可写时信号埋点丢弃（信号路径无日志系统可用） */         \
        }                                                                            \
    } while (0)
#else
#define DAEMON_SIG_RECEIVED_TRACE(daemon_name) ((void)0)
#endif

#define DAEMON_DECLARE_COMMON(daemon_name, daemon_cname, DEFAULT_SOCKET_PATH_UNIX,   \
                              DEFAULT_SOCKET_PATH_WIN, DEFAULT_TCP_PORT, MAX_BUFFER) \
                                                                                     \
    static atomic_int g_running_##daemon_name = 1;                                   \
    static airy_mtx_t g_running_lock_##daemon_name;                              \
    static method_dispatcher_t *g_dispatcher_##daemon_name = NULL;                    \
    static daemon_event_driver_t *g_event_driver_##daemon_name = NULL;                \
    static daemon_bootstrap_sd_t *g_bsd_##daemon_name = NULL;                         \
    static daemon_bootstrap_ipc_t *g_bipc_##daemon_name = NULL;                       \
                                                                                     \
    static void signal_handler_##daemon_name(int sig)                                 \
    {                                                                                \
        (void)sig;                                                                   \
        /* 信号处理器必须保持 async-signal-safe：不锁互斥锁、不调用日志系统，         \
         * 仅做原子置位 + eventfd 异步安全唤醒，避免与日志锁死锁导致无法退出。       \
         * 由 daemon_event_driver_stop_async 保持纯异步安全路径。                    */ \
        atomic_store_explicit(&g_running_##daemon_name, 0, memory_order_seq_cst);     \
        if (g_event_driver_##daemon_name)                                            \
            daemon_event_driver_stop_async(g_event_driver_##daemon_name);            \
        /* 关键节点埋点：信号接收（write 为 async-signal-safe，绕过日志系统           \
         * 避免锁死，输出到 stderr 便于排查）                                       */ \
        DAEMON_SIG_RECEIVED_TRACE(daemon_name);                                        \
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
    __attribute__((unused))                                                              \
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
                 AIRY_CONFIG_DIR "/" #daemon_name ".yaml\n", prog);                  \
        fputs(buf, stdout);                                                          \
        snprintf(buf, sizeof(buf), "  %s --tcp  # TCP mode on port %d\n",             \
                 prog, DEFAULT_TCP_PORT);                                             \
        fputs(buf, stdout);                                                          \
    }                                                                                \
                                                                                     \
    __attribute__((unused))                                                              \
    static int daemon_handle_client_##daemon_name(                                    \
        airy_sock_t client_fd, method_dispatcher_t *dispatcher)                  \
    {                                                                                \
        char buffer[MAX_BUFFER];                                                     \
        /* 等待请求数据就绪后再 recv。accept 返回的 fd 由事件驱动在 epoll 可读      \
         * 事件中 accept，但数据可能尚未到达；airy_sock_recv 为 MSG_DONTWAIT 非阻塞  \
         * 读取，立即 recv 会因 EAGAIN 返回 0 而误判连接失败并关闭，导致客户端       \
         * send 时 SIGPIPE/请求丢失（RPC 时序竞态）。此处先 poll 等待 POLLIN。       \
         * 5s 超时与客户端 daemon_rpc_call 默认 30s 相比足够，超时视为请求丢失。   */ \
        if (AIRY_PLATFORM_POSIX) {                                                  \
            struct pollfd pfd;                                                       \
            pfd.fd = (int)client_fd;                                                 \
            pfd.events = POLLIN;                                                     \
            pfd.revents = 0;                                                         \
            int pr = poll(&pfd, 1, 5000);                                            \
            if (pr <= 0 || !(pfd.revents & POLLIN)) {                                \
                airy_sock_close(client_fd);                                          \
                return AIRY_ERR_FAIL;                                                \
            }                                                                        \
        }                                                                            \
        ssize_t n = airy_sock_recv(client_fd, buffer, sizeof(buffer) - 1);      \
        if (n <= 0) {                                                                \
            airy_sock_close(client_fd);                                         \
            return AIRY_ERR_FAIL;                                                 \
        }                                                                            \
        buffer[n] = '\0';                                                            \
        if ((size_t)n >= sizeof(buffer) - 1) {                                       \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST,                    \
                               "Request too large", -1);                              \
            airy_sock_close(client_fd);                                         \
            return AIRY_ERR_FAIL;                                                 \
        }                                                                            \
        /* P0.18.2: 模式 A — CJSON_PARSE_GUARD 自动释放 + NULL 检查 */                \
        CJSON_PARSE_GUARD(req, buffer, {                                             \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_PARSE_ERROR,                        \
                               "Parse error: invalid JSON", -1);                      \
            airy_sock_close(client_fd);                                         \
            return AIRY_ERR_FAIL;                                                 \
        });                                                                          \
        if (getenv("AIRY_DAEMON_DUMP_REQ")) {                                        \
            __builtin_fprintf(stderr, "[AIRY-DUMP] %s req[%zd]=\"%.400s\"\n",        \
                              #daemon_name, n, buffer);                              \
        }                                                                            \
        cJSON *jsonrpc = cJSON_GetObjectItem(req, "jsonrpc");                        \
        cJSON *method = cJSON_GetObjectItem(req, "method");                          \
        cJSON *id = cJSON_GetObjectItem(req, "id");                                  \
        if (!cJSON_IsString(jsonrpc) ||                                              \
            strcmp(jsonrpc->valuestring, "2.0") != 0 ||                              \
            !cJSON_IsString(method) || !id) {                                        \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST,                    \
                               "Invalid Request", -1);                               \
            /* req 由 CJSON_AUTO_FREE 自动释放 */                                    \
            airy_sock_close(client_fd);                                         \
            return AIRY_ERR_FAIL;                                                 \
        }                                                                            \
        int req_id = cJSON_IsNumber(id) ? id->valueint : 0;                          \
        SVC_LOG_DEBUG("Processing request: method=%s, id=%d",                        \
                      method->valuestring, req_id);                                   \
        int dr = method_dispatcher_dispatch(dispatcher, req, jsonrpc_build_error,    \
                                            &client_fd);                             \
        if (dr != 0) {                                                               \
            /* dispatch 的错误路径只构建错误字符串不发送（历史缺陷），此处补发，     \
               避免客户端收到空响应/EOF 无从诊断。 */                                \
            if (dr == AIRY_ERR_NOT_FOUND) {                                           \
                JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND,              \
                                   "Method not found", req_id);                      \
            } else if (dr == AIRY_ERR_PARSE_ERROR) {                                 \
                JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST,               \
                                   "Invalid request", req_id);                       \
            } else {                                                                 \
                JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,                \
                                   "Internal error", req_id);                        \
            }                                                                        \
        }                                                                            \
        /* req 由 CJSON_AUTO_FREE 自动释放 */                                        \
        airy_sock_close(client_fd);                                             \
        return 0;                                                                    \
    }                                                                                \
                                                                                     \
    __attribute__((unused))                                                              \
    static int daemon_on_client_##daemon_name(                                        \
        void *service_ctx, airy_sock_t client_fd)                                \
    {                                                                                \
        (void)service_ctx;                                                           \
        return daemon_handle_client_##daemon_name(client_fd,                           \
                           g_dispatcher_##daemon_name);                               \
    }


/* ==================== 公共初始化辅助函数 ==================== */

/**
 * @brief 生成标准 L2 <ns>.shutdown 方法处理器（02-l2-service-protocol.md §6.1）
 *
 * 该方法与信号处理路径保持一致：原子置位 g_running_<daemon> + 唤醒事件驱动
 * （daemon_event_driver_stop_async），使主循环优雅退出（真实生效，非桩），
 * 随后向调用方返回成功响应。
 *
 * 使用方式（main.c）：
 *   1. 在 DAEMON_DECLARE_COMMON(...) 之后（文件顶层）展开本宏生成处理器；
 *   2. 在 register_rpc_methods() 中注册：
 *        method_dispatcher_register(g_dispatcher_<daemon>, "shutdown",
 *                                   on_shutdown_method_<daemon>, NULL);
 *
 * @note user_data 由 daemon_handle_client_<daemon> 传入 &client_fd，
 *       因此 *(airy_sock_t *)user_data 为客户端 socket。
 */
#define DAEMON_DECLARE_SHUTDOWN_METHOD(daemon_name)                                  \
    static void on_shutdown_method_##daemon_name(cJSON *params, int id, void *user_data) \
    {                                                                                \
        (void)params;                                                                \
        /* 与信号处理器一致：原子置位 + 事件驱动异步唤醒（async-signal-safe 路径）*/  \
        atomic_store_explicit(&g_running_##daemon_name, 0, memory_order_seq_cst);    \
        if (g_event_driver_##daemon_name)                                            \
            daemon_event_driver_stop_async(g_event_driver_##daemon_name);            \
        cJSON *result = cJSON_CreateObject();                                        \
        cJSON_AddStringToObject(result, "status", "shutting_down");                  \
        JSONRPC_SEND_SUCCESS(*(airy_sock_t *)user_data, result, id);                 \
        SVC_LOG_INFO("RPC shutdown requested, initiating graceful shutdown");         \
    }


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
static inline airy_sock_t daemon_create_server_socket(
    int use_tcp, int tcp_port,
    const char *unix_path, const char *win_pipe)
{
    if (use_tcp) {
        return airy_sock_create_tcp_server("127.0.0.1", tcp_port);
    }
#if defined(AIRY_PLATFORM_WINDOWS)
    (void)unix_path;
    return airy_sock_create_named_pipe_server(win_pipe);
#else
    (void)win_pipe;
    return airy_sock_create_unix_server(unix_path);
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
 * @return             AIRY_SUCCESS 或错误码
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
    if (!ev_config || !p_event_driver) return AIRY_ERR_INVALID_PARAM;
    *p_event_driver = daemon_event_driver_create(ev_config);
    if (!*p_event_driver) return AIRY_ERR_OUT_OF_MEMORY;

    return AIRY_SUCCESS;
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
    airy_sock_t server_fd,
    void (*destroy_service)(void),
    airy_mtx_t *running_lock)
{
    SVC_LOG_WARN("Service stopping...");

    if (bipc)  daemon_bootstrap_ipc_stop(bipc);
    if (bsd)   daemon_bootstrap_sd_stop(bsd);
    if (event_driver) daemon_event_driver_destroy(event_driver);
    if (server_fd >= 0) airy_sock_close(server_fd);
    if (destroy_service) destroy_service();
    if (running_lock) airy_mtx_destroy(running_lock);
    airy_sock_cleanup();

    SVC_LOG_WARN("Service stopped");
}


#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_MAIN_H */
