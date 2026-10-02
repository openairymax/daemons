/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_main.h
 * @brief Daemon 引导机制层：符号声明宏 + 启动策略包类型。
 *
 * P0.18.1: eliminates duplicated boilerplate across the daemon main.c
 * files (about 5,956 -> about 1,500 lines).
 *
 * 0.1.19 §79（机制/策略分界定稿）:
 * - main() 装配骨架收敛为机制函数 daemon_boot()（实现:
 *   src/daemon/daemon_boot.c）；各户 main.c 只保留策略面——
 *   DAEMON_DECLARE_COMMON 符号声明 + daemon_boot_t 策略包填充。
 * - 装配细节（参数解析/信号注册/套接字创建/事件驱动引导/标准清理链）
 *   为机制，实现随 daemon_boot.c 私有；本头只暴露策略包类型与
 *   per-daemon 符号生成宏。
 *
 * @see ARCHITECTURAL_PRINCIPLES.md E-3~E-6
 */

#ifndef AIRY_RT_DAEMON_MAIN_H
#define AIRY_RT_DAEMON_MAIN_H

#include "daemon_bootstrap_ipc.h"
#include "daemon_bootstrap_sd.h"
#include "daemon_cfg_file.h"
#include "daemon_cupolas_bootstrap.h"
#include "daemon_event_driver.h"
#include "daemon_l1_server.h"
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
    static inline void signal_handler_##daemon_name(int sig)                                                       \
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
    static inline void svc_log_toggle_handler_##daemon_name(int sig)                                               \
    {                                                                                                              \
        (void)sig;                                                                                                 \
        static int debug_mode = 0;                                                                                 \
        debug_mode = !debug_mode;                                                                                  \
        log_set_module_level("*", debug_mode ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);                                  \
    }                                                                                                              \
                                                                                                                   \
    static inline void print_usage_##daemon_name(const char *prog)                                                 \
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
    static inline int daemon_handle_request_json_##daemon_name(                                   \
        const char *req_text, size_t req_len, method_dispatcher_t *dispatcher, airy_sock_t client_fd)              \
    {                                                                                                              \
        /* Parse + validate + dispatch, then emit the response through the  \
         * JSONRPC_SEND_* macros (routed to the thread's response sink when \
         * one is active, else written to client_fd). This function never   \
         * closes client_fd: the socket path closes it in                   \
         * daemon_handle_client_<daemon>, while the corekern path passes -1 \
         * and owns no fd at all (airy_sock_close(-1) is not called). */     \
        /* P0.18.2: mode A - CJSON_PARSE_GUARD auto-free + NULL check */                                              \
        CJSON_PARSE_GUARD(req, req_text, {                                                                           \
            AIRY_FREE(req_text);                                                                                     \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_PARSE_ERROR, "Parse error: invalid JSON", -1);                     \
            return AIRY_ERR_GENERIC_FAIL;                                                                            \
        });                                                                                                          \
        if (getenv("AIRY_DAEMON_DUMP_REQ")) {                                                                        \
            __builtin_fprintf(stderr, "[AIRY-DUMP] %s req[%zu]=\"%.400s\"\n", #daemon_name, req_len,                 \
                              req_text);                                                                             \
        }                                                                                                            \
        AIRY_FREE(req_text);                                                                                        \
        cJSON *jsonrpc = cJSON_GetObjectItem(req, "jsonrpc");                                                       \
        cJSON *method = cJSON_GetObjectItem(req, "method");                                                         \
        cJSON *id = cJSON_GetObjectItem(req, "id");                                                                 \
        if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0 ||                                 \
            !cJSON_IsString(method) || !id) {                                                                       \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST, "Invalid Request", -1);                          \
            /* req is auto-freed by CJSON_AUTO_FREE */                                                              \
            return AIRY_ERR_GENERIC_FAIL;                                                                            \
        }                                                                                                           \
        int req_id = cJSON_IsNumber(id) ? id->valueint : 0;                                                         \
        SVC_LOG_DEBUG("Processing request: method=%s, id=%d", method->valuestring, req_id);                         \
        int dr = method_dispatcher_dispatch(dispatcher, req, jsonrpc_build_error, &client_fd);                      \
        if (dr != 0) {                                                                                              \
            /* dispatch's error path only builds the error string without  \
             * sending it (historical defect); resend here so the client   \
             * does not receive an empty response/EOF with no diagnosis. */\
            if (dr == AIRY_ERR_NOT_FOUND) {                                                                         \
                JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Method not found",                        \
                                   req_id);                                                                        \
            } else if (dr == AIRY_ERR_PARSE_ERROR) {                                                                \
                JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST, "Invalid request", req_id);                 \
            } else {                                                                                                \
                JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Internal error", req_id);                   \
            }                                                                                                       \
        }                                                                                                           \
        /* req is auto-freed by CJSON_AUTO_FREE */                                                                  \
        return 0;                                                                                                   \
    }                                                                                                              \
                                                                                                                   \
    static inline int daemon_handle_client_##daemon_name(                                         \
        airy_sock_t client_fd, method_dispatcher_t *dispatcher)                                                    \
    {                                                                                                              \
        /* Read the full JSON-RPC request frame. airy_daemon_read_request   \
         * loop-poll+recv's probing JSON completeness (mirroring the         \
         * client-side rpc_recv_response), so requests larger than the old  \
         * 64 KiB single-recv cap are no longer truncated and rejected.     \
         * This was a real failure path: llm.complete after a tool loop     \
         * feeds back large web_fetch results, grew past 64 KiB, got        \
         * truncated into a JSON parse error, and the final answer round    \
         * was silently killed (daily chat showed tool cards but never      \
         * streamed the final answer). */                                    \
        size_t req_len = 0;                                                                                         \
        const char *req_err = NULL;                                                                                 \
        char *req_text = airy_daemon_read_request(client_fd, &req_len, &req_err);                                    \
        if (!req_text) {                                                                                            \
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_REQUEST,                                                   \
                               req_err ? req_err : "Request read failed", -1);                                      \
            airy_sock_close(client_fd);                                                                            \
            return AIRY_ERR_GENERIC_FAIL;                                                                                  \
        }                                                                                                          \
        int rc = daemon_handle_request_json_##daemon_name(req_text, req_len, dispatcher, client_fd);                \
        /* req_text is owned and freed by handle_request_json (all paths); \
         * the fd stays ours: single-request-single-response-then-close. */ \
        airy_sock_close(client_fd);                                                                                \
        return rc;                                                                                                 \
    }                                                                                                              \
                                                                                                                   \
    static inline int daemon_on_client_##daemon_name(void *service_ctx,                    \
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
    static inline void on_shutdown_method_##daemon_name(cJSON *params, int id, void *user_data) \
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
 * @brief Table-driven method registration (mechanism, complements the
 *        generated main.c of daemon_gen.py).
 *
 * Hand-written if-else registration chains are the last manual boilerplate
 * left in daemon mains. The generated main.c declares a static
 * daemon_method_entry_t table (manifest-driven SSoT) and registers the
 * whole table in one call, keeping the entry file within its line budget
 * regardless of method count.
 */
typedef struct {
    const char *name;
    method_fn handler;
} daemon_method_entry_t;

/**
 * @brief X-macro entry expander for the manifest-derived method list.
 *
 * The generated svc_<d>.h owns the SVC_<D>_METHODS(X) list (single source
 * of truth, derived from .manifest rpc.methods); main.c expands it into the
 * daemon_method_entry_t table. This keeps the entry file within its line
 * budget regardless of method count.
 */
#define DAEMON_METHOD_ENTRY(name, fn) { (name), (fn) },

/* ops 引导条目：词表 ipc/llm/tool（daemon_gen.py OPS_VOCAB），init 正序、
 * cleanup 逆序由 daemon_boot 机制序执行。 */
typedef struct {
    airy_err_t (*init)(const char *daemon);
    void (*cleanup)(void);
} daemon_op_t;

/**
 * @brief 启动策略包：daemon_gen.py render_main 生成的 per-daemon 策略面。
 *
 * 符号面字段引用 DAEMON_DECLARE_COMMON 在各户 main.c 展开的 static
 * 符号地址；策略面字段承载 .manifest 派生的户间差异（端点/事件池/
 * ops 集/cupolas 模式/svc 钩子/方法表）。装配序、错误路径与清理链
 * 为机制，收敛在 daemon_boot()。
 */
typedef struct {
    const char *daemon;     /* 进程/SD 引导名（如 "monit_d"） */
    const char *cname;      /* 服务名（日志与 namespace 文案，如 "monitor"） */
    const char *env_debug;  /* 调试日志开关环境变量名 */
    const char *sd_type;    /* 服务发现类型 */
    const char *tags;       /* 服务发现标签 */
    int method_total;       /* 注册方法数日志（含协议保留 shutdown） */
    /* DAEMON_DECLARE_COMMON 符号面（main.c 内 static，地址传入） */
    airy_mtx_t *running_lock;
    void (*signal_handler)(int);
    void (*log_toggle)(int);
    void (*print_usage)(const char *);
    daemon_on_client_cb on_client;
    method_dispatcher_t **dispatcher;
    daemon_event_driver_t **event_driver;
    daemon_bootstrap_sd_t **bsd;
    daemon_bootstrap_ipc_t **bipc;
    /* 事件池策略（.manifest rpc.pool） */
    int pool_max_events;
    int pool_min;
    int pool_max;
    int pool_queue;
    int concurrent_clients;
    /* ops 引导集（词表 ipc/llm/tool；空集置 NULL/0） */
    const daemon_op_t *ops;
    size_t ops_count;
    /* cupolas 引导策略：pep=daemon_cupolas_init_pep，full=daemon_cupolas_init */
    airy_err_t (*cupolas_init)(const char *daemon);
    /* svc 策略钩子（实现: 各户 src/svc.c） */
    int (*svc_prepare)(const char *config_path);
    void (*svc_endpoint)(daemon_endpoint_t *ep, int cmdline_tcp);
    int (*svc_activate)(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd);
    void (*svc_attach)(void *dispatcher);
    void (*svc_teardown)(void);
    void (*svc_destroy)(void);
    /* 方法表（SVC_<D>_METHODS 展开，含协议保留 shutdown） */
    const daemon_method_entry_t *methods;
    size_t method_count;
} daemon_boot_t;

/**
 * @brief 入口接线模板：svc 六钩子与两张静态表的恒定连线（SSoT）。
 *
 * 钩子名十二户恒定（DAEMON_DECLARE_COMMON 约定面），唯 ops 表、方法表
 * 与 cupolas 模式随户异；以模板宏收敛生成户 main.c 的横向接线副本
 * （0.1.19 §79），装配机制仍整体在 daemon_boot()。
 *
 * 形参顺序（ops_, methods_, activate_, cupolas_init_）：两张静态表
 * 在前，激活钩子策略居第三。activate_ 为激活钩子策略：实体户传
 * svc_activate（src/svc.c），无激活策略户传 daemon_svc_noop 缺省
 * （0.1.19 §80，svc.c 不再逐户维护空桩）。
 */
#define DAEMON_BOOT_WIRE(ops_, methods_, activate_, cupolas_init_)             \
    .ops = (ops_), .ops_count = sizeof(ops_) / sizeof((ops_)[0]),              \
    .cupolas_init = (cupolas_init_),                                           \
    .svc_prepare = svc_prepare, .svc_endpoint = svc_endpoint,                  \
    .svc_activate = (activate_), .svc_attach = svc_attach,                     \
    .svc_teardown = svc_teardown, .svc_destroy = svc_destroy,                  \
    .methods = (methods_),                                                     \
    .method_count = sizeof(methods_) / sizeof((methods_)[0])

/**
 * @brief svc 激活钩子缺省策略（null object，0.1.19 §80）。
 *
 * 无激活策略户经 DAEMON_BOOT_WIRE 第三参引用本符号，机制层单点
 * 提供空激活语义；签名对齐 daemon_boot_t::svc_activate。实现:
 * src/daemon/daemon_boot.c。
 */
int daemon_svc_noop(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd);

/**
 * @brief Daemon 启动机制：parse -> init -> serve -> cleanup 全装配序。
 *
 * 实现（src/daemon/daemon_boot.c）逐行承载原生成 main() 装配骨架；
 * 返回值为进程退出码。错误路径 fail_driver/fail_svc 与原模板一致。
 */
int daemon_boot(int argc, char **argv, const daemon_boot_t *boot);

/**
 * @brief Opt a daemon into the corekern same-process transport (blueprint
 *        8.3.3, three-path replacement).
 *
 * Expands (after DAEMON_DECLARE_COMMON(daemon_name, ...)) into:
 *   - g_l2_bridge_<daemon_name>: the mounted bridge handle (NULL = off)
 *   - daemon_l2_dispatch_<daemon_name>: the L2 dispatch trampoline. It
 *     installs a thread-local response sink, reuses the exact socket-path
 *     request handling (daemon_handle_request_json_<daemon_name>) with
 *     client_fd = -1, and hands the captured response to the bridge. Every
 *     JSONRPC_SEND_ERROR path (parse/validate/dispatch failures) therefore
 *     reaches the caller as a structured JSON-RPC error object instead of
 *     a bare CANCELED.
 *   - daemon_l2_mount_<daemon_name>(dispatcher): transport gate
 *     (AIRY_<NS_UPPER>_IPC_TRANSPORT, default off) + bridge start. Returns
 *     AIRY_ERR_NOT_FOUND when the transport is off so main() can log and
 *     stay on the socket path with zero behavior change.
 *   - daemon_l2_unmount_<daemon_name>(void): drain + free (stop is NULL-
 *     safe), called from the daemon cleanup path.
 *
 * @param daemon_name Daemon token (must match DAEMON_DECLARE_COMMON)
 * @param ns_upper    Transport-switch namespace, UPPER_SNAKE, derived from
 *                    the socket basename (sched.sock -> SCHED, monit.sock
 *                    -> MONIT; NOT the service cname)
 * @param ns_lower    Channel namespace (sched.sock -> "sched" -> channel
 *                    "sched.rpc")
 */
#define DAEMON_L2_ENABLE(daemon_name, ns_upper, ns_lower)                                               \
    static daemon_l2_bridge_t *g_l2_bridge_##daemon_name = NULL;                                        \
                                                                                                        \
    static inline int daemon_l2_dispatch_##daemon_name(                                \
        const char *req_json, size_t req_len, char **resp_json, size_t *resp_len, void *userdata)       \
    {                                                                                                   \
        method_dispatcher_t *dispatcher = (method_dispatcher_t *)userdata;                              \
        /* The bridge hands a length-bounded payload view with no NUL      \
         * guarantee, while handle_request_json parses via cJSON_Parse    \
         * and unconditionally AIRY_FREEs the request text (socket-path  \
         * ownership). Materialize an owned NUL-terminated copy so both  \
         * contracts hold: the parser sees '\0' and the free sees a      \
         * malloc'd base instead of an envelope interior pointer. */     \
        char *req_copy = (char *)AIRY_MALLOC(req_len + 1);                                              \
        if (!req_copy)                                                                                  \
            return AIRY_ERR_OUT_OF_MEMORY;                                                              \
        AIRY_MEMCPY(req_copy, req_json, req_len);                                                       \
        req_copy[req_len] = '\0';                                                                       \
        jsonrpc_resp_sink_t sink = {0};                                                                 \
        jsonrpc_resp_sink_activate(&sink);                                                              \
        (void)daemon_handle_request_json_##daemon_name(req_copy, req_len, dispatcher, -1);              \
        jsonrpc_resp_sink_deactivate();                                                                 \
        if (!sink.buf) {                                                                                \
            /* No captured response (sink OOM): fail the transaction so the \
             * caller surfaces CANCELED instead of an empty reply. */       \
            return AIRY_ERR_GENERIC_FAIL;                                                               \
        }                                                                                               \
        *resp_json = sink.buf; /* AIRY_MALLOC domain; the bridge frees */                               \
        *resp_len = sink.len;                                                                           \
        return 0;                                                                                       \
    }                                                                                                   \
                                                                                                        \
    static inline int daemon_l2_mount_##daemon_name(method_dispatcher_t *dispatcher)   \
    {                                                                                                   \
        if (!daemon_l1_transport_enabled(#ns_upper)) {                                                  \
            SVC_LOG_INFO("l2 %s: transport off, staying on sockets", #ns_lower);                        \
            return AIRY_ERR_NOT_FOUND;                                                                  \
        }                                                                                               \
        g_l2_bridge_##daemon_name =                                                                     \
            daemon_l2_bridge_start(#ns_lower ".rpc", daemon_l2_dispatch_##daemon_name, dispatcher);     \
        if (!g_l2_bridge_##daemon_name) {                                                               \
            SVC_LOG_ERROR("l2 %s: bridge start failed, staying on sockets", #ns_lower);                 \
            return AIRY_ERR_UNKNOWN;                                                                    \
        }                                                                                               \
        SVC_LOG_INFO("l2 %s: mounted corekern channel " #ns_lower ".rpc", #ns_lower);                   \
        return 0;                                                                                       \
    }                                                                                                   \
                                                                                                        \
    static inline void daemon_l2_unmount_##daemon_name(void)                    \
    {                                                                                                   \
        daemon_l2_bridge_stop(g_l2_bridge_##daemon_name);                                               \
        g_l2_bridge_##daemon_name = NULL;                                                               \
    }

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_MAIN_H */
