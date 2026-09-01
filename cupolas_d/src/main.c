// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "airy_memory.h"
#include "error.h"
/*
 * @file main.c
 * @brief Cupolas security-dome service daemon main entry (daemon module
 *        conventions).
 *
 * cupolas_d splits the cupolas security library into a standalone daemon,
 * exposing JSON-RPC methods (cupolas.* namespace):
 *   - cupolas.check_permission : permission decision (1 allow / 0 deny)
 *   - cupolas.sanitize         : input sanitization
 *   - cupolas.execute_command  : isolated workbench command execution
 *   - cupolas.add_rule         : dynamically add a permission rule
 *   - cupolas.audit_flush      : flush audit logs
 *   - cupolas.get_stats        : service stats (real counters)
 *   - cupolas.shutdown         : graceful exit
 *
 * cupolas_d itself hosts the cupolas security library: main() initializes
 * the security dome (permission engine + input sanitization + audit logs +
 * daemon_security) via daemon_cupolas_init("cupolas_d"), and flushes the
 * audit log via daemon_cupolas_cleanup() before exit.
 *
 * Unix socket path: ${AIRY_RUNTIME_DIR}/cupolas.sock
 *
 * 2026-08-27 域拆分（原 995 行 → 4 文件）：本文件仅保留入口引导——daemon
 * 宏样板、daemon 配置装配与 main() 事件驱动循环；RPC 方法按域拆分为
 * cupolas_rpc_core.c（权限/净化/命令/规则/审计/健康/统计）、
 * cupolas_rpc_vault.c（vault.*）、cupolas_rpc_net_entitlements.c
 * （net.* / entitlements.*），共享符号经 cupolas_d_internal.h 声明。
 */

#include "daemon_main.h"
#include "platform.h"
#include "cupolas_d_internal.h"
#include "dynamic_policy_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("cupolas.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_cupolas"
#define DEFAULT_TCP_PORT 8089
#define MAX_BUFFER 65536
#define MAX_CLIENTS 64

DAEMON_DECLARE_COMMON(cupolas_d, cupolas, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(cupolas_d)

cupolas_service_t *g_service = NULL;

/* PDP：动态策略引擎（M2-S3，唯一策略持有者）。创建/销毁于 main() */
dpolicy_engine_t *g_dpolicy = NULL;

typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_clients;
} cupolas_daemon_config_t;

static cupolas_daemon_config_t g_config = {0};

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler_cupolas_d((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static int load_daemon_config(const char *config_path)
{

    g_config.use_tcp = 0;
    g_config.max_clients = MAX_CLIENTS;

#if defined(AIRY_PLATFORM_WINDOWS)
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_WIN);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#else
    g_config.socket_path = AIRY_STRDUP(DEFAULT_SOCKET_PATH_UNIX);
    g_config.tcp_host = AIRY_STRDUP("127.0.0.1");
#endif
    g_config.tcp_port = DEFAULT_TCP_PORT;

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
                                if (cJSON_IsNumber(tcp_port)) {
                                    g_config.tcp_port = (uint16_t)tcp_port->valueint;
                                    g_config.use_tcp = 1;
                                }

                                cJSON *max_clients = cJSON_GetObjectItem(daemon_cfg, "max_clients");
                                if (cJSON_IsNumber(max_clients)) {
                                    g_config.max_clients = max_clients->valueint;
                                }
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
        cupolas_service_destroy(g_service);
        g_service = NULL;
    }
    if (g_dpolicy) {
        dpolicy_engine_destroy(g_dpolicy);
        g_dpolicy = NULL;
    }
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    int use_tcp = 0;

    int parse_rc = daemon_parse_args(argc, argv, &config_path, &use_tcp, print_usage_cupolas_d);
    if (parse_rc > 0)
        return parse_rc == 1 ? 0 : 1;

    airy_sock_init();
    airy_mtx_init(&g_running_lock_cupolas_d);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    DAEMON_SETUP_SIGNALS(cupolas_d);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    /* cupolas_d itself hosts the cupolas security library: initialize the
     * security dome (permission_engine + sanitizer + audit_logger +
     * daemon_security) */
    daemon_cupolas_init("cupolas_d");

    /* PDP：动态策略引擎（M2-S3）——cupolas_d 作为唯一策略持有者 */
    g_dpolicy = dpolicy_engine_create(DPOLICY_CONFLICT_DENY_WINS);
    if (!g_dpolicy) {
        SVC_LOG_ERROR("Failed to create dynamic policy engine");
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_cupolas_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    load_daemon_config(config_path);
    if (use_tcp)
        g_config.use_tcp = 1;

    SVC_LOG_INFO("Cupolas service starting, manager=%s", config_path ? config_path : "default");

    g_service = cupolas_service_create(config_path);
    if (!g_service) {
        SVC_LOG_ERROR("Failed to create cupolas service");
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_cupolas_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    airy_sock_t server_fd = daemon_create_server_socket(g_config.use_tcp, g_config.tcp_port,
                                                        g_config.socket_path, g_config.socket_path);
    if (server_fd < 0) {
        SVC_LOG_ERROR("Failed to create server socket");
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_cupolas_d);
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
    ev_config.on_client = daemon_on_client_cupolas_d;
    ev_config.service_ctx = NULL;

    const char *sock_addr = g_config.use_tcp ? g_config.tcp_host : g_config.socket_path;
    int ret = daemon_init_event_driver("cupolas_d", "cupolas", sock_addr,
                                       g_config.use_tcp ? g_config.tcp_port : 0, "cupolas,security",
                                       g_config.use_tcp, &ev_config, &g_event_driver_cupolas_d,
                                       &g_bsd_cupolas_d, &g_bipc_cupolas_d);
    if (ret != AIRY_SUCCESS || !g_event_driver_cupolas_d) {
        SVC_LOG_ERROR("Failed to create event driver");
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_cupolas_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    g_dispatcher_cupolas_d = daemon_event_driver_get_dispatcher(g_event_driver_cupolas_d);

    /* 方法注册点驱动（SSoT）：计数源自注册本身，避免硬编码漂移 */
    size_t g_method_count = 0;
#define REG_RPC(disp, name, fn)                                             \
    do {                                                                    \
        method_dispatcher_register((disp), (name), (fn), NULL);             \
        g_method_count++;                                                   \
    } while (0)

    REG_RPC(g_dispatcher_cupolas_d, "check_permission", on_check_permission_method);
    REG_RPC(g_dispatcher_cupolas_d, "sanitize", on_sanitize_method);
    REG_RPC(g_dispatcher_cupolas_d, "execute_command", on_execute_command_method);
    REG_RPC(g_dispatcher_cupolas_d, "add_rule", on_add_rule_method);
    REG_RPC(g_dispatcher_cupolas_d, "audit_flush", on_audit_flush_method);
    /* L2 protocol standard methods (02-l2-service-protocol.md:
     * cupolas.health_check / cupolas.get_stats / cupolas.shutdown) */
    REG_RPC(g_dispatcher_cupolas_d, "health_check", on_health_check_method);
    REG_RPC(g_dispatcher_cupolas_d, "get_stats", on_get_stats_method);
    REG_RPC(g_dispatcher_cupolas_d, "shutdown", on_shutdown_method_cupolas_d);

    REG_RPC(g_dispatcher_cupolas_d, "vault_store", on_vault_store_method);
    REG_RPC(g_dispatcher_cupolas_d, "vault_retrieve", on_vault_retrieve_method);
    REG_RPC(g_dispatcher_cupolas_d, "vault_delete", on_vault_delete_method);
    REG_RPC(g_dispatcher_cupolas_d, "vault_list", on_vault_list_method);
    REG_RPC(g_dispatcher_cupolas_d, "vault_rotate", on_vault_rotate_method);
    REG_RPC(g_dispatcher_cupolas_d, "net_add_rule", on_net_add_rule_method);
    REG_RPC(g_dispatcher_cupolas_d, "net_check_access", on_net_check_access_method);
    REG_RPC(g_dispatcher_cupolas_d, "net_get_stats", on_net_get_stats_method);
    REG_RPC(g_dispatcher_cupolas_d, "entitlements_load", on_entitlements_load_method);
    REG_RPC(g_dispatcher_cupolas_d, "entitlements_check", on_entitlements_check_method);
    /* M2-S3 (0.1.9 §3.2 PDP)：策略演化——cupolas_d 唯一策略持有者 */
    REG_RPC(g_dispatcher_cupolas_d, "policy_load", on_policy_load_method);
    REG_RPC(g_dispatcher_cupolas_d, "policy_activate", on_policy_activate_method);
    REG_RPC(g_dispatcher_cupolas_d, "policy_rollback", on_policy_rollback_method);
    REG_RPC(g_dispatcher_cupolas_d, "policy_status", on_policy_status_method);

#undef REG_RPC
    SVC_LOG_INFO("Registered %zu RPC methods (cupolas.* / policy.* namespace)",
                 g_method_count);

    if (daemon_event_driver_add_server_fd(g_event_driver_cupolas_d, (int)server_fd) != 0) {
        SVC_LOG_ERROR("Failed to add server fd to event driver");
        daemon_event_driver_destroy(g_event_driver_cupolas_d);
        airy_sock_close(server_fd);
        destroy_service();
        free_daemon_config();
        airy_mtx_destroy(&g_running_lock_cupolas_d);
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("Cupolas service running (event-driven mode)");
    daemon_event_driver_run(g_event_driver_cupolas_d);

    daemon_cleanup_standard(g_bipc_cupolas_d, g_bsd_cupolas_d, g_event_driver_cupolas_d, server_fd,
                            g_config.socket_path, destroy_service, &g_running_lock_cupolas_d);
    free_daemon_config();

    SVC_LOG_INFO("Cupolas service stopped");
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
