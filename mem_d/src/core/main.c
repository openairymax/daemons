// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "airy_rt.h"
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
    /* B5：L2 压缩门禁默认 fail-closed（L2 关、灰度关、acr/ttft 不可用） */
    g_config.compress_l1_enabled = 1;
    g_config.compress_l2_enabled = 0;
    g_config.compress_gate_grayscale = 0;
    g_config.compress_gate_acr = -1.0;
    g_config.compress_gate_ttft_ms = -1.0;
    /* B5：台账计数模型默认空（由 token_standard 默认模型兜底） */
    g_config.token_model[0] = '\0';

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

    /* B5：压缩门禁策略可用环境变量翻转（策略值改动不触发重编译） */
    const char *env_l2 = getenv("AIRY_MEM_COMPRESS_L2");
    if (env_l2)
        g_config.compress_l2_enabled = strtoul(env_l2, NULL, 10) != 0 ? 1 : 0;
    const char *env_gs = getenv("AIRY_MEM_COMPRESS_GATE_GRAYSCALE");
    if (env_gs)
        g_config.compress_gate_grayscale = strtoul(env_gs, NULL, 10) != 0 ? 1 : 0;
    const char *env_acr = getenv("AIRY_MEM_COMPRESS_GATE_ACR");
    if (env_acr) {
        char *end = NULL;
        double v = strtod(env_acr, &end);
        if (end != env_acr)
            g_config.compress_gate_acr = v;
    }
    const char *env_ttft = getenv("AIRY_MEM_COMPRESS_GATE_TTFT_MS");
    if (env_ttft) {
        char *end = NULL;
        double v = strtod(env_ttft, &end);
        if (end != env_ttft)
            g_config.compress_gate_ttft_ms = v;
    }

    /* B5：台账计数模型（按实际模型取，缺省回退默认模型） */
    const char *env_model = getenv("AIRY_MEM_TOKEN_MODEL");
    if (env_model && env_model[0])
        AIRY_STRNCPY_TERM(g_config.token_model, env_model, sizeof(g_config.token_model));

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
                            /* B5：压缩门禁策略声明（缺省字段保持 fail-closed 初值） */
                            cJSON *ccfg = cJSON_GetObjectItem(root, "compress");
                            if (ccfg) {
                                cJSON *l1 = cJSON_GetObjectItem(ccfg, "l1_enabled");
                                if (cJSON_IsBool(l1))
                                    g_config.compress_l1_enabled = cJSON_IsTrue(l1) ? 1 : 0;
                                cJSON *l2 = cJSON_GetObjectItem(ccfg, "l2_enabled");
                                if (cJSON_IsBool(l2))
                                    g_config.compress_l2_enabled = cJSON_IsTrue(l2) ? 1 : 0;
                                cJSON *gate = cJSON_GetObjectItem(ccfg, "gate");
                                if (gate) {
                                    cJSON *gs = cJSON_GetObjectItem(gate, "grayscale");
                                    if (cJSON_IsBool(gs))
                                        g_config.compress_gate_grayscale = cJSON_IsTrue(gs) ? 1 : 0;
                                    cJSON *acr = cJSON_GetObjectItem(gate, "acr");
                                    if (cJSON_IsNumber(acr))
                                        g_config.compress_gate_acr = acr->valuedouble;
                                    cJSON *ttft = cJSON_GetObjectItem(gate, "ttft_ms");
                                    if (cJSON_IsNumber(ttft))
                                        g_config.compress_gate_ttft_ms = ttft->valuedouble;
                                }
                            }
                            /* B5：台账计数模型声明（缺省 → 默认模型，不得硬编码） */
                            cJSON *tcfg = cJSON_GetObjectItem(root, "token");
                            if (tcfg) {
                                cJSON *model = cJSON_GetObjectItem(tcfg, "model");
                                if (cJSON_IsString(model) && model->valuestring[0])
                                    AIRY_STRNCPY_TERM(g_config.token_model, model->valuestring,
                                                      sizeof(g_config.token_model));
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

    /* WS-8 stage 4 (8.4.1): bring up the corekern core (mem/oom/task/ipc/
     * eventloop/persist) as the first link of the daemon boot chain, before
     * the daemon's own subsystems. airy_init() is idempotent; if it fails
     * the daemon still runs on the platform fallbacks (DSL degradation,
     * non-fatal, badge=0). */
    {
        int core_ret = airy_init();
        if (core_ret == AIRY_SUCCESS) {
            SVC_LOG_INFO("corekern core initialized (mem_d runs on corekern)");
        } else {
            SVC_LOG_WARN("corekern init failed (%d) - running degraded (badge=0)", core_ret);
        }
    }

    daemon_cupolas_init("mem_d");

    load_daemon_config(config_path);
    if (use_tcp)
        g_config.use_tcp = 1;

    SVC_LOG_INFO("Memory service starting, manager=%s", config_path ? config_path : "default");
    SVC_LOG_INFO("compress policy: l1=%d l2=%d gate{grayscale=%d acr=%.3f ttft=%.1fms}",
                 g_config.compress_l1_enabled, g_config.compress_l2_enabled,
                 g_config.compress_gate_grayscale, g_config.compress_gate_acr,
                 g_config.compress_gate_ttft_ms);

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
    if (!g_ledger) {
        SVC_LOG_WARN("Context ledger init failed, ledger disabled (degraded mode)");
    } else {
        /* B5：按声明注入计数模型（空 → 默认模型），失败保留默认模型 */
        int model_ret = mem_ledger_set_token_model(g_ledger, g_config.token_model);
        if (model_ret != AIRY_SUCCESS)
            SVC_LOG_WARN("Token model '%s' rejected (%d), fallback kept: %s",
                         g_config.token_model[0] ? g_config.token_model : "(default)", model_ret,
                         mem_ledger_token_model(g_ledger));
        SVC_LOG_INFO("ledger token model: %s", mem_ledger_token_model(g_ledger));
    }

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
