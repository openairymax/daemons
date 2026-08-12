/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_main.h
 * @brief Daemon main() boilerplate macros and common helpers.
 *
 * P0.18.1: eliminates duplicated boilerplate across the 12 daemon main.c
 * files (about 5,956 -> about 1,500 lines).
 *
 * Architecture (P1.23 + daemon_startup.h):
 * - The 12 daemon main() structures are essentially identical:
 *   init -> create -> run -> cleanup
 * - The only differences: service-create function, method registration
 *   table, service-destroy function
 * - This file extracts the identical boilerplate into inline functions;
 *   the differences are injected via template macros
 *
 * Usage:
 *   In each daemon's main.c, you only need to:
 *   1. define service global variables
 *   2. define method handlers
 *   3. call DAEMON_MAIN_BOILERPLATE() to generate main()
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


/**
 * @brief Generate the common global variables and signal-handler
 *        declarations for a daemon main.c.
 *
 * @param daemon_name  Daemon process name (e.g. "sched_d")
 * @param daemon_cname Daemon config and service name (e.g. "scheduler")
 *
 * Generates variables:
 *   - static atomic_int g_running = 1
 *   - static airy_mtx_t g_running_lock
 *   - static method_dispatcher_t *g_dispatcher = NULL
 *   - static daemon_event_driver_t *g_event_driver = NULL
 *   - static daemon_bootstrap_sd_t *g_bsd = NULL
 *   - static daemon_bootstrap_ipc_t *g_bipc = NULL
 *
 * Generates functions:
 *   - static void signal_handler(int sig)
 *   - static void svc_log_toggle_handler(int sig)
 *   - static void print_usage(const char *prog, const char *service_name)
 *   - static int daemon_handle_client(daemon_event_driver_t *driver,
 *         airy_sock_t client_fd, method_dispatcher_t *dispatcher)
 */
/* SIGTERM receipt trace: write directly to stderr (async-signal-safe,
 * bypasses the logging lock). The platform conditional must be outside
 * the macro - preprocessor directives (#if/#endif) cannot appear inside a
 * macro body, or '#' is taken as the stringize operator, causing "'#' is
 * not followed by a macro parameter". */
#if AIRY_PLATFORM_POSIX
#define DAEMON_SIG_RECEIVED_TRACE(daemon_name)                                                 \
    do {                                                                                       \
        static const char _sig_msg_##daemon_name[] =                                           \
            "[SIG] shutdown signal received, initiating graceful shutdown\n";                  \
        if (write(STDERR_FILENO, _sig_msg_##daemon_name, sizeof(_sig_msg_##daemon_name) - 1) < \
            0) {                                                                               \
            /* Ignored: signal trace dropped when stderr is unwritable    */                  \
        }                                                                                      \
    } while (0)
#else
#define DAEMON_SIG_RECEIVED_TRACE(daemon_name) ((void)0)
#endif

#define DAEMON_DECLARE_COMMON(daemon_name, daemon_cname, DEFAULT_SOCKET_PATH_UNIX,                                 \
                              DEFAULT_SOCKET_PATH_WIN, DEFAULT_TCP_PORT, MAX_BUFFER)                               \
                                                                                                                   \
    static atomic_int g_running_##daemon_name = 1;                                                                 \
    static airy_mtx_t g_running_lock_##daemon_name;                                                                \
    static method_dispatcher_t *g_dispatcher_##daemon_name = NULL;                                                 \
    static daemon_event_driver_t *g_event_driver_##daemon_name = NULL;                                             \
    static daemon_bootstrap_sd_t *g_bsd_##daemon_name = NULL;                                                      \
    static daemon_bootstrap_ipc_t *g_bipc_##daemon_name = NULL;                                                    \
                                                                                                                   \
    static void signal_handler_##daemon_name(int sig)                                                              \
    {                                                                                                              \
        (void)sig;                                                                                                 \
        /* Signal handlers must stay async-signal-safe: no mutex locks,     \
         * no logging calls - only an atomic flag set plus eventfd async-   \
         * safe wakeup, avoiding deadlock with the logging lock on exit.    \
         * daemon_event_driver_stop_async keeps the path purely async-safe.*/\
        atomic_store_explicit(&g_running_##daemon_name, 0, memory_order_seq_cst);                                  \
        if (g_event_driver_##daemon_name)                                                                          \
            daemon_event_driver_stop_async(g_event_driver_##daemon_name);                                          \
        /* Key-point trace: signal received (write is async-signal-safe,     \
         * bypasses the logging system to avoid deadlock; stderr for easy    \
         * diagnosis)                                                       */\
        DAEMON_SIG_RECEIVED_TRACE(daemon_name);                                                                    \
    }                                                                                                              \
                                                                                                                   \
    static void svc_log_toggle_handler_##daemon_name(int sig)                                                      \
    {                                                                                                              \
        (void)sig;                                                                                                 \
        static int debug_mode = 0;                                                                                 \
        debug_mode = !debug_mode;                                                                                  \
        log_set_module_level("*", debug_mode ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);                                  \
    }                                                                                                              \
                                                                                                                   \
    __attribute__((unused)) static void print_usage_##daemon_name(const char *prog)                                \
    {                                                                                                              \
        char buf[256];                                                                                             \
        fputs("AgentRT " #daemon_name " (" #daemon_cname ")\n", stdout);                                           \
        snprintf(buf, sizeof(buf), "Usage: %s [options]\n\n", prog);                                               \
        fputs(buf, stdout);                                                                                        \
        fputs("Options:\n", stdout);                                                                               \
        fputs("  --manager <path>   Configuration file path\n", stdout);                                           \
        fputs("  --tcp             Use TCP instead of Unix socket\n", stdout);                                     \
        fputs("  --help             Show this help\n", stdout);                                                    \
        fputs("\n", stdout);                                                                                       \
        fputs("Examples:\n", stdout);                                                                              \
        snprintf(buf, sizeof(buf), "  %s --manager " AIRY_CONFIG_DIR "/" #daemon_name ".yaml\n",                   \
                 prog);                                                                                            \
        fputs(buf, stdout);                                                                                        \
        snprintf(buf, sizeof(buf), "  %s --tcp  # TCP mode on port %d\n", prog, DEFAULT_TCP_PORT);                 \
        fputs(buf, stdout);                                                                                        \
    }                                                                                                              \
                                                                                                                   \
    __attribute__((unused)) static int daemon_handle_client_##daemon_name(                                         \
        airy_sock_t client_fd, method_dispatcher_t *dispatcher)                                                    \
    {                                                                                                              \
        char buffer[MAX_BUFFER];                                                                                   \
        /* Wait for request data before recv. The fd accepted by the event  \
         * driver may not have data yet; airy_sock_recv is a MSG_DONTWAIT   \
         * non-blocking read, so an immediate recv can return 0 on EAGAIN,   \
         * be mistaken for a failed connection and closed, causing SIGPIPE / \
         * lost requests on the client send (RPC timing race). Poll for     \
         * POLLIN first. 5s timeout is ample vs the client daemon_rpc_call  \
         * default of 30s; on timeout the request is treated as lost.      */\
        if (AIRY_PLATFORM_POSIX) {                                                                                 \
            struct pollfd pfd;                                                                                     \
            pfd.fd = (int)client_fd;                                                                               \
            pfd.events = POLLIN;                                                                                   \
            pfd.revents = 0;                                                                                       \
            int pr = poll(&pfd, 1, 5000);                                                                          \
            if (pr <= 0 || !(pfd.revents & POLLIN)) {                                                              \
                airy_sock_close(client_fd);                                                                        \
                return AIRY_ERR_FAIL;                                                                              \
            }                                                                                                      \
        }                                                                                                          \
        ssize_t n = airy_sock_recv(client_fd, buffer, sizeof(buffer) - 1);                                         \
        if (n <= 0) {                                                                                              \
            airy_sock_close(client_fd);                                                                            \
            return AIRY_ERR_FAIL;                                                                                  \
        }                                                                                                          \
        buffer[n] = '\0';                                                                                          \
        if ((size_t)n >= sizeof(buffer) - 1) {                                                                     \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST, "Request too large", -1);                       \
            airy_sock_close(client_fd);                                                                            \
            return AIRY_ERR_FAIL;                                                                                  \
        }                                                                                                          \
        /* P0.18.2: mode A - CJSON_PARSE_GUARD auto-free + NULL check */                                             \
        CJSON_PARSE_GUARD(req, buffer, {                                                                           \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_PARSE_ERROR, "Parse error: invalid JSON", -1);                   \
            airy_sock_close(client_fd);                                                                            \
            return AIRY_ERR_FAIL;                                                                                  \
        });                                                                                                        \
        if (getenv("AIRY_DAEMON_DUMP_REQ")) {                                                                      \
            __builtin_fprintf(stderr, "[AIRY-DUMP] %s req[%zd]=\"%.400s\"\n", #daemon_name, n,                     \
                              buffer);                                                                             \
        }                                                                                                          \
        cJSON *jsonrpc = cJSON_GetObjectItem(req, "jsonrpc");                                                      \
        cJSON *method = cJSON_GetObjectItem(req, "method");                                                        \
        cJSON *id = cJSON_GetObjectItem(req, "id");                                                                \
        if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0 ||                                \
            !cJSON_IsString(method) || !id) {                                                                      \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST, "Invalid Request", -1);                         \
            /* req is auto-freed by CJSON_AUTO_FREE */                                                            \
            airy_sock_close(client_fd);                                                                            \
            return AIRY_ERR_FAIL;                                                                                  \
        }                                                                                                          \
        int req_id = cJSON_IsNumber(id) ? id->valueint : 0;                                                        \
        SVC_LOG_DEBUG("Processing request: method=%s, id=%d", method->valuestring, req_id);                        \
        int dr = method_dispatcher_dispatch(dispatcher, req, jsonrpc_build_error, &client_fd);                     \
        if (dr != 0) {                                                                                             \
            /* dispatch's error path only builds the error string without  \
             * sending it (historical defect); resend here so the client   \
             * does not receive an empty response/EOF with no diagnosis. */\
            if (dr == AIRY_ERR_NOT_FOUND) {                                                                        \
                JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Method not found",                        \
                                   req_id);                                                                        \
            } else if (dr == AIRY_ERR_PARSE_ERROR) {                                                               \
                JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST, "Invalid request", req_id);                 \
            } else {                                                                                               \
                JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Internal error", req_id);                   \
            }                                                                                                      \
        }                                                                                                          \
        /* req is auto-freed by CJSON_AUTO_FREE */                                                                \
        airy_sock_close(client_fd);                                                                                \
        return 0;                                                                                                  \
    }                                                                                                              \
                                                                                                                   \
    __attribute__((unused)) static int daemon_on_client_##daemon_name(void *service_ctx,                           \
                                                                      airy_sock_t client_fd)                       \
    {                                                                                                              \
        (void)service_ctx;                                                                                         \
        return daemon_handle_client_##daemon_name(client_fd, g_dispatcher_##daemon_name);                          \
    }


/**
 * @brief Generate the standard L2 <ns>.shutdown method handler
 *        (02-l2-service-protocol.md §6.1).
 *
 * This method stays consistent with the signal-handling path: atomically
 * clears g_running_<daemon> + wakes the event driver
 * (daemon_event_driver_stop_async), so the main loop exits gracefully
 * (really effective, not a stub), then returns a success response to the
 * caller.
 *
 * Usage (main.c):
 *   1. Expand this macro after DAEMON_DECLARE_COMMON(...) (file top level)
 *      to generate the handler;
 *   2. Register it in register_rpc_methods():
 *        method_dispatcher_register(g_dispatcher_<daemon>, "shutdown",
 *                                   on_shutdown_method_<daemon>, NULL);
 *
 * @note user_data is passed &client_fd by daemon_handle_client_<daemon>,
 *       so *(airy_sock_t *)user_data is the client socket.
 */
#define DAEMON_DECLARE_SHUTDOWN_METHOD(daemon_name)                                      \
    static void on_shutdown_method_##daemon_name(cJSON *params, int id, void *user_data) \
    {                                                                                    \
        (void)params;                                                                    \
        /* Same as the signal handler: atomic flag + event-driver async-   \
         * wakeup (async-signal-safe path)                                  */          \
        atomic_store_explicit(&g_running_##daemon_name, 0, memory_order_seq_cst);        \
        if (g_event_driver_##daemon_name)                                                \
            daemon_event_driver_stop_async(g_event_driver_##daemon_name);                \
        cJSON *result = cJSON_CreateObject();                                            \
        cJSON_AddStringToObject(result, "status", "shutting_down");                      \
        JSONRPC_SEND_SUCCESS(*(airy_sock_t *)user_data, result, id);                     \
        SVC_LOG_INFO("RPC shutdown requested, initiating graceful shutdown");            \
    }

/**
 * @brief Cross-platform signal setup.
 *
 * @param daemon_name Used to generate unique signal-handler function names
 *
 * Same in every daemon:
 * - POSIX: SIGINT/SIGTERM -> signal_handler, SIGPIPE -> IGN, SIGUSR1 -> svc_log_toggle
 * - Windows: SetConsoleCtrlHandler -> console_handler
 */
#define DAEMON_SETUP_SIGNALS(daemon_name)                      \
    do {                                                       \
        signal(SIGINT, signal_handler_##daemon_name);          \
        signal(SIGTERM, signal_handler_##daemon_name);         \
        signal(SIGPIPE, SIG_IGN);                              \
        signal(SIGUSR1, svc_log_toggle_handler_##daemon_name); \
    } while (0)

/**
 * @brief Command-line argument parsing (--manager, --tcp, --help).
 *
 * @param config_path Config-path pointer, will be modified
 * @param use_tcp     TCP flag pointer, will be modified
 *
 * Return value: 0=continue, >0=exit(code), <0=error
 */
static inline int daemon_parse_args(int argc, char **argv, const char **config_path, int *use_tcp,
                                    void (*print_usage_fn)(const char *))
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--manager") == 0 && i + 1 < argc) {
            *config_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            if (print_usage_fn)
                print_usage_fn(argv[0]);
            return 1; /* exit(0) */
        } else if (strcmp(argv[i], "--tcp") == 0) {
            *use_tcp = 1;
        } else {
            SVC_LOG_ERROR("Unknown option: %s", argv[i]);
            if (print_usage_fn)
                print_usage_fn(argv[0]);
            return 2; /* exit(1) */
        }
    }
    return 0;
}

/**
 * @brief Create a server socket (TCP or Unix).
 *
 * Unifies TCP/Unix socket creation, removing duplicated branches in each
 * daemon.
 *
 * @param use_tcp     Whether to use TCP
 * @param tcp_port    TCP port (used when use_tcp=true)
 * @param unix_path   Unix Socket path (used when use_tcp=false)
 * @param win_pipe    Windows Named Pipe path
 * @return            Socket fd on success, < 0 on failure
 */
static inline airy_sock_t daemon_create_server_socket(int use_tcp, int tcp_port,
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
 * @brief Create and start the Event Driver and Bootstrap services.
 *
 * @param daemon_name  Daemon name (e.g. "sched_d")
 * @param service_type Service-discovery type (e.g. "scheduler")
 * @param socket_path  Socket path (Unix) or "127.0.0.1" (TCP)
 * @param tcp_port     TCP port (0=Unix)
 * @param tags         Service tags (e.g. "scheduler,core")
 * @param use_tcp      Whether TCP mode
 * @param ev_config    Event-driver config
 * @param p_event_driver Output: event-driver instance
 * @param p_bsd        Output: SD bootstrap instance
 * @param p_bipc       Output: IPC bootstrap instance
 * @return             AIRY_SUCCESS or error code
 */
static inline int daemon_init_event_driver(const char *daemon_name, const char *service_type,
                                           const char *socket_path, int tcp_port, const char *tags,
                                           int use_tcp, const daemon_event_config_t *ev_config,
                                           daemon_event_driver_t **p_event_driver,
                                           daemon_bootstrap_sd_t **p_bsd,
                                           daemon_bootstrap_ipc_t **p_bipc)
{

    if (p_bsd) {
        const char *sd_addr = use_tcp ? "127.0.0.1" : socket_path;
        *p_bsd = daemon_bootstrap_sd_start(daemon_name, service_type, sd_addr, tcp_port, tags, 0);
    }

    /* IPC bootstrap */
    if (p_bipc) {
        const char *ipc_addr = use_tcp ? "127.0.0.1" : socket_path;
        *p_bipc = daemon_bootstrap_ipc_start(daemon_name, service_type, ipc_addr, tcp_port,
                                             IPC_BUS_PROTO_JSON_RPC);
    }


    if (!ev_config || !p_event_driver)
        return AIRY_ERR_INVALID_PARAM;
    *p_event_driver = daemon_event_driver_create(ev_config);
    if (!*p_event_driver)
        return AIRY_ERR_OUT_OF_MEMORY;

    return AIRY_SUCCESS;
}

/**
 * @brief Standard daemon resource cleanup chain.
 *
 * Cleans up all resources in reverse order of init:
 *   bootstrap_ipc -> bootstrap_sd -> event_driver -> socket -> service -> mutex -> socket_cleanup -> cupolas -> log
 */
static inline void daemon_cleanup_standard(daemon_bootstrap_ipc_t *bipc, daemon_bootstrap_sd_t *bsd,
                                           daemon_event_driver_t *event_driver,
                                           airy_sock_t server_fd, void (*destroy_service)(void),
                                           airy_mtx_t *running_lock)
{
    SVC_LOG_WARN("Service stopping...");

    if (bipc)
        daemon_bootstrap_ipc_stop(bipc);
    if (bsd)
        daemon_bootstrap_sd_stop(bsd);
    if (event_driver)
        daemon_event_driver_destroy(event_driver);
    if (server_fd >= 0)
        airy_sock_close(server_fd);
    if (destroy_service)
        destroy_service();
    if (running_lock)
        airy_mtx_destroy(running_lock);
    airy_sock_cleanup();

    SVC_LOG_WARN("Service stopped");
}

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_MAIN_H */
