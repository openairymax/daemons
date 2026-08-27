// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file main.c
 * @brief Memory service daemon main entry (daemon module conventions).
 *
 * Exposes JSON-RPC methods (mem.* namespace):
 *   - mem.write   : write a memory record
 *   - mem.search  : keyword search
 *   - mem.get     : read by ID
 *   - mem.delete  : delete by ID
 *   - mem.count   : current record count (health-check helper)
 *
 * Unix socket path: ${AIRY_RUNTIME_DIR}/mem.sock
 *
 * Refactored: handler logic lives in dedicated modules (mem_handlers,
 * kb_handlers, cache_handlers, ledger_handlers); this file owns the
 * daemon lifecycle, RPC callback dispatch, and method registration.
 */

#include "daemon_main.h"
#include "platform.h"
#include "mem_service.h"
#include "cache.h"
#include "ledger.h"
#include "compress.h"
#include "param_validator.h"
#include "svc_logger.h"
#include "thread_pool.h"

/* Handler modules (extracted from the former monolithic main.c) */
#include "mem_daemon_ctx.h"
#include "mem_handlers.h"
#include "kb_handlers.h"
#include "cache_handlers.h"
#include "ledger_handlers.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("mem.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_mem"
#define DEFAULT_TCP_PORT 8085
#define MAX_BUFFER 65536

/* ── Global service instances ────────────────────────────────────────── */

mem_service_t *g_service = NULL;
/* 语义缓存 + 上下文台账（0.1.5：13-semantic-cache-context-ledger.md 实现） */
mem_cache_t *g_cache = NULL;
mem_ledger_t *g_ledger = NULL;

mem_daemon_config_t g_config = {0};

/* ── Daemon framework declarations ───────────────────────────────────── */

DAEMON_DECLARE_COMMON(mem_d, mem, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(mem_d)

/* ── Windows console handler ─────────────────────────────────────────── */

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_mem_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

/* ── RPC callback wrappers ─────────────────────────────────────────────
 * Each on_*_method adapts the void* user_data to the airy_sock_t that
 * the handler expects.  Kept as thin trampolines — no business logic. */

static void on_write_method(cJSON *params, int id, void *user_data)
{ handle_write(params, id, *(airy_sock_t *)user_data); }

static void on_search_method(cJSON *params, int id, void *user_data)
{ handle_search(params, id, *(airy_sock_t *)user_data); }

static void on_get_method(cJSON *params, int id, void *user_data)
{ handle_get(params, id, *(airy_sock_t *)user_data); }

static void on_delete_method(cJSON *params, int id, void *user_data)
{ handle_delete(params, id, *(airy_sock_t *)user_data); }

static void on_count_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{ handle_count(id, *(airy_sock_t *)user_data); }

static void on_recent_method(cJSON *params, int id, void *user_data)
{ handle_recent(params, id, *(airy_sock_t *)user_data); }

static void on_evolve_method(cJSON *params, int id, void *user_data)
{ handle_evolve(params, id, *(airy_sock_t *)user_data); }

static void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{ handle_health_check(id, *(airy_sock_t *)user_data); }

static void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{ handle_get_stats(id, *(airy_sock_t *)user_data); }

static void on_kb_ingest_method(cJSON *params, int id, void *user_data)
{ handle_kb_ingest(params, id, *(airy_sock_t *)user_data); }

static void on_kb_search_method(cJSON *params, int id, void *user_data)
{ handle_kb_search(params, id, *(airy_sock_t *)user_data); }

static void on_kb_delete_method(cJSON *params, int id, void *user_data)
{ handle_kb_delete(params, id, *(airy_sock_t *)user_data); }

static void on_kb_list_method(cJSON *params, int id, void *user_data)
{ handle_kb_list(params, id, *(airy_sock_t *)user_data); }

static void on_cache_put_method(cJSON *params, int id, void *user_data)
{ handle_cache_put(params, id, *(airy_sock_t *)user_data); }

static void on_cache_get_method(cJSON *params, int id, void *user_data)
{ handle_cache_get(params, id, *(airy_sock_t *)user_data); }

static void on_cache_del_method(cJSON *params, int id, void *user_data)
{ handle_cache_del(params, id, *(airy_sock_t *)user_data); }

static void on_cache_stats_method(cJSON *params, int id, void *user_data)
{ handle_cache_stats(id, *(airy_sock_t *)user_data); }

static void on_ledger_append_method(cJSON *params, int id, void *user_data)
{ handle_ledger_append(params, id, *(airy_sock_t *)user_data); }

static void on_ledger_window_method(cJSON *params, int id, void *user_data)
{ handle_ledger_window(params, id, *(airy_sock_t *)user_data); }

static void on_ledger_budget_method(cJSON *params, int id, void *user_data)
{ handle_ledger_budget(params, id, *(airy_sock_t *)user_data); }

static void on_ledger_mark_method(cJSON *params, int id, void *user_data)
{ handle_ledger_mark(params, id, *(airy_sock_t *)user_data); }

static void on_ledger_history_method(cJSON *params, int id, void *user_data)
{ handle_ledger_history(params, id, *(airy_sock_t *)user_data); }

static void on_ledger_stats_method(cJSON *params, int id, void *user_data)
{ handle_ledger_stats(id, *(airy_sock_t *)user_data); }

static void on_compress_method(cJSON *params, int id, void *user_data)
{ handle_compress(params, id, *(airy_sock_t *)user_data); }

/* ── Configuration ───────────────────────────────────────────────────── */

static int load_daemon_config(const char *config_path)
{
    g_config.use_tcp = 0;
    g_config.max_clients = MAX_CLIENTS;
    g_config.max_records = MEM_DEFAULT_MAX_RECORDS;

#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_WIN);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#else
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_UNIX);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#endif
    g_config.tcp_port = DEFAULT_TCP_PORT;

    const char *env = getenv("AIRY_MEM_MAX_RECORDS");
    if (env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v > 0 && v < 65536)
            g_config.max_records = (size_t)v;
    }

    if (config_path) {
        FILE *f = fopen(config_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len > 0 && len < 1024 * 1024) {
                char *content = (char *)AIRY_MALLOC((size_t)len + 1);
                if (content) {
                    size_t read_len = fread(content, 1, (size_t)len, f);
                    if (read_len == (size_t)len) {
                        content[read_len] = '\0';
                        do {
                            CJSON_PARSE_GUARD(root, content, { break; });
                            cJSON *daemon_cfg = cJSON_GetObjectItem(root, "daemon");
                            if (daemon_cfg) {
                                cJSON *socket_path = cJSON_GetObjectItem(daemon_cfg, "socket_path");
                                if (cJSON_IsString(socket_path)) {
                                    AIRY_FREE(g_config.socket_path);
                                    g_config.socket_path = AIRY_STRDUP(socket_path->valuestring);
                                }
                                cJSON *tcp_port = cJSON_GetObjectItem(daemon_cfg, "tcp_port");
                                if (cJSON_IsNumber(tcp_port) && tcp_port->valueint > 0 &&
                                    tcp_port->valueint <= 65535) {
                                    g_config.tcp_port = (uint16_t)tcp_port->valueint;
                                    g_config.use_tcp = 1;
                                }
                                cJSON *max_clients = cJSON_GetObjectItem(daemon_cfg, "max_clients");
                                if (cJSON_IsNumber(max_clients))
                                    g_config.max_clients = max_clients->valueint;
                                cJSON *max_records = cJSON_GetObjectItem(daemon_cfg, "max_records");
                                if (cJSON_IsNumber(max_records))
                                    g_config.max_records = (size_t)max_records->valuedouble;
                            }
                        } while (0);
                    }
                    AIRY_FREE(content);
                }
            }
            fclose(f);
        }
    }
    return 0;
}

static void free_daemon_config(void)
{
    AIRY_FREE(g_config.socket_path);
    AIRY_FREE(g_config.tcp_host);
    __builtin_memset(&g_config, 0, sizeof(g_config));
}

static void destroy_service(void)
{
    if (g_service) {
        mem_service_destroy(g_service);
        g_service = NULL;
    }
    if (g_cache) {
        mem_cache_destroy(g_cache);
        g_cache = NULL;
    }
    if (g_ledger) {
        mem_ledger_destroy(g_ledger);
        g_ledger = NULL;
    }
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_mem_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_mem_d);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    DAEMON_SETUP_SIGNALS(mem_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    daemon_cupolas_init("mem_d");

    load_daemon_config(config_path);
    if (use_tcp)
        g_config.use_tcp = 1;

    SVC_LOG_INFO("Memory service starting, manager=%s", config_path ? config_path : "default");

    g_service = mem_service_create(g_config.max_records);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create memory service");
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_mem_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    /* 0.1.5：语义缓存 + 上下文台账（渐进式降级：创建失败仅告警，不阻断服务） */
    g_cache = mem_cache_create(4096, 64UL * 1024 * 1024, 3600000ULL, 0.85);
    if (!g_cache)
        SVC_LOG_WARN("Semantic cache init failed, caching disabled (degraded mode)");
    g_ledger = mem_ledger_create(0, 0);
    if (!g_ledger)
        SVC_LOG_WARN("Context ledger init failed, ledger disabled (degraded mode)");

    airy_sock_t server_fd = daemon_create_server_socket(g_config.use_tcp, g_config.tcp_port,
                                                        g_config.socket_path, g_config.socket_path);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_mem_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }
    SVC_LOG_INFO(g_config.use_tcp ? "Listening on TCP %s:%d" : "Listening on %s", g_config.tcp_host,
                 g_config.tcp_port);

    daemon_event_config_t ev_config;
    __builtin_memset(&ev_config, 0, sizeof(ev_config));
    ev_config.max_events = 64;
    ev_config.thread_pool_min = 4;
    ev_config.thread_pool_max = 8;
    ev_config.thread_pool_queue_size = 256;
    ev_config.use_jsonrpc = true;
    ev_config.on_client = daemon_on_client_mem_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = g_config.use_tcp ? g_config.tcp_host : g_config.socket_path;
    int ret = daemon_init_event_driver("mem_d", "mem", sock_addr,
                                       g_config.use_tcp ? g_config.tcp_port : 0, "mem,core",
                                       g_config.use_tcp, &ev_config, &g_event_driver_mem_d,
                                       &g_bsd_mem_d, &g_bipc_mem_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_mem_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_mem_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_mem_d = daemon_event_driver_get_dispatcher(g_event_driver_mem_d);
    method_dispatcher_register(g_dispatcher_mem_d, "write", on_write_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "search", on_search_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "get", on_get_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "delete", on_delete_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "count", on_count_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "recent", on_recent_method, NULL);

    method_dispatcher_register(g_dispatcher_mem_d, "evolve", on_evolve_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "health_check", on_health_check_method, NULL);

    method_dispatcher_register(g_dispatcher_mem_d, "shutdown", on_shutdown_method_mem_d, NULL);

    method_dispatcher_register(g_dispatcher_mem_d, "get_stats", on_get_stats_method, NULL);

    /* 2.1.2.3：KB 知识库（RAG 一等抽象） */
    method_dispatcher_register(g_dispatcher_mem_d, "kb_ingest", on_kb_ingest_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "kb_search", on_kb_search_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "kb_delete", on_kb_delete_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "kb_list", on_kb_list_method, NULL);

    /* 0.1.5：语义缓存（13-semantic-cache-context-ledger.md §3） */
    method_dispatcher_register(g_dispatcher_mem_d, "cache_put", on_cache_put_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "cache_get", on_cache_get_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "cache_del", on_cache_del_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "cache_stats", on_cache_stats_method, NULL);

    /* 0.1.5：上下文台账（13-semantic-cache-context-ledger.md §4） */
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_append", on_ledger_append_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_window", on_ledger_window_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_budget", on_ledger_budget_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_mark", on_ledger_mark_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_history", on_ledger_history_method, NULL);
    method_dispatcher_register(g_dispatcher_mem_d, "ledger_stats", on_ledger_stats_method, NULL);

    /* 0.1.5：提示词压缩（14-prompt-compression.md §3 L1+L2） */
    method_dispatcher_register(g_dispatcher_mem_d, "compress", on_compress_method, NULL);
    SVC_LOG_INFO("Registered 25 RPC methods (mem.* namespace)");

    if (daemon_event_driver_add_server_fd(g_event_driver_mem_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_mem_d);
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_mem_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("Memory service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_mem_d);

    daemon_cleanup_standard(g_bipc_mem_d, g_bsd_mem_d, g_event_driver_mem_d, server_fd,
                            g_config.socket_path, destroy_service, &g_running_lock_mem_d);
    free_daemon_config();

    SVC_LOG_INFO("Memory service stopped");
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
