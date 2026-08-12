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
 */

#include "daemon_main.h"
#include "platform.h"
#include "param_validator.h"
#include "svc_logger.h"
#include "thread_pool.h"
#include "cupolas_service.h"
#include "cupolas_vault.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char HEX_CHARS[] = "0123456789abcdef";

static char *hex_encode(const uint8_t *data, size_t len)
{
    if (!data || len == 0)
        return NULL;
    char *out = (char *)AIRY_MALLOC(len * 2 + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = HEX_CHARS[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = HEX_CHARS[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
    return out;
}

static uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (uint8_t)(c - 'A' + 10);
    return 0xFF;
}

static uint8_t *hex_decode(const char *hex, size_t *out_len)
{
    if (!hex || !out_len)
        return NULL;
    size_t len = strlen(hex);
    if (len % 2 != 0)
        return NULL;
    uint8_t *out = (uint8_t *)AIRY_MALLOC(len / 2);
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i += 2) {
        uint8_t hi = hex_nibble(hex[i]);
        uint8_t lo = hex_nibble(hex[i + 1]);
        if (hi == 0xFF || lo == 0xFF) {
            AIRY_FREE(out);
            *out_len = 0;
            return NULL;
        }
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = len / 2;
    return out;
}

#define DEFAULT_SOCKET_PATH_UNIX airy_runtime_dir_socket("cupolas.sock")
#define DEFAULT_SOCKET_PATH_WIN "\\\\.\\pipe\\airy_cupolas"
#define DEFAULT_TCP_PORT 8089
#define MAX_BUFFER 65536
#define MAX_CLIENTS 64

DAEMON_DECLARE_COMMON(cupolas_d, cupolas, DEFAULT_SOCKET_PATH_UNIX, DEFAULT_SOCKET_PATH_WIN,
                      DEFAULT_TCP_PORT, MAX_BUFFER)

DAEMON_DECLARE_SHUTDOWN_METHOD(cupolas_d)

static cupolas_service_t *g_service = NULL;

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

static void handle_check_permission(cJSON *params, int id, airy_sock_t fd);
static void handle_sanitize(cJSON *params, int id, airy_sock_t fd);
static void handle_execute_command(cJSON *params, int id, airy_sock_t fd);
static void handle_add_rule(cJSON *params, int id, airy_sock_t fd);
static void handle_audit_flush(cJSON *params, int id, airy_sock_t fd);
static void handle_get_stats(int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);
static void handle_vault_store(cJSON *params, int id, airy_sock_t fd);
static void handle_vault_retrieve(cJSON *params, int id, airy_sock_t fd);
static void handle_vault_delete(cJSON *params, int id, airy_sock_t fd);
static void handle_vault_list(cJSON *params, int id, airy_sock_t fd);
static void handle_vault_rotate(cJSON *params, int id, airy_sock_t fd);
static void handle_net_add_rule(cJSON *params, int id, airy_sock_t fd);
static void handle_net_check_access(cJSON *params, int id, airy_sock_t fd);
static void handle_net_get_stats(int id, airy_sock_t fd);
static void handle_entitlements_load(cJSON *params, int id, airy_sock_t fd);
static void handle_entitlements_check(cJSON *params, int id, airy_sock_t fd);

static void on_check_permission_method(cJSON *params, int id, void *user_data)
{
    handle_check_permission(params, id, *(airy_sock_t *)user_data);
}

static void on_sanitize_method(cJSON *params, int id, void *user_data)
{
    handle_sanitize(params, id, *(airy_sock_t *)user_data);
}

static void on_execute_command_method(cJSON *params, int id, void *user_data)
{
    handle_execute_command(params, id, *(airy_sock_t *)user_data);
}

static void on_add_rule_method(cJSON *params, int id, void *user_data)
{
    handle_add_rule(params, id, *(airy_sock_t *)user_data);
}

static void on_audit_flush_method(cJSON *params, int id, void *user_data)
{
    handle_audit_flush(params, id, *(airy_sock_t *)user_data);
}

static void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

static void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
}

/* cupolas.vault_store */
static void on_vault_store_method(cJSON *params, int id, void *user_data)
{
    handle_vault_store(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.vault_retrieve */
static void on_vault_retrieve_method(cJSON *params, int id, void *user_data)
{
    handle_vault_retrieve(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.vault_delete */
static void on_vault_delete_method(cJSON *params, int id, void *user_data)
{
    handle_vault_delete(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.vault_list */
static void on_vault_list_method(cJSON *params, int id, void *user_data)
{
    handle_vault_list(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.vault_rotate */
static void on_vault_rotate_method(cJSON *params, int id, void *user_data)
{
    handle_vault_rotate(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.net_add_rule */
static void on_net_add_rule_method(cJSON *params, int id, void *user_data)
{
    handle_net_add_rule(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.net_check_access */
static void on_net_check_access_method(cJSON *params, int id, void *user_data)
{
    handle_net_check_access(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.net_get_stats */
static void on_net_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_net_get_stats(id, *(airy_sock_t *)user_data);
}

/* cupolas.entitlements_load */
static void on_entitlements_load_method(cJSON *params, int id, void *user_data)
{
    handle_entitlements_load(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.entitlements_check */
static void on_entitlements_check_method(cJSON *params, int id, void *user_data)
{
    handle_entitlements_check(params, id, *(airy_sock_t *)user_data);
}

static void handle_check_permission(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *agent_id = get_string_field(params, "agent_id", NULL);
    const char *action = get_string_field(params, "action", NULL);
    const char *resource = get_string_field(params, "resource", NULL);
    const char *context = get_string_field(params, "context", NULL);

    if (!agent_id || !action || !resource) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "agent_id, action and resource required", id);
        return;
    }

    cupolas_check_permission_params_t req = {.agent_id = agent_id,
                                             .action = action,
                                             .resource = resource,
                                             .context = context};
    cupolas_check_permission_result_t res = {0};

    int ret = cupolas_service_check_permission(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Permission check failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "allowed", res.allowed ? true : false);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_sanitize(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *input = get_string_field(params, "input", NULL);
    if (!input) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "input required", id);
        return;
    }

    cupolas_sanitize_params_t req = {.input = input};
    cupolas_sanitize_result_t res = {0};

    int ret = cupolas_service_sanitize(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {

        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Input rejected by sanitizer", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "sanitized", res.sanitized ? res.sanitized : "");
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    cupolas_sanitize_result_free(&res);
}

static void handle_execute_command(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *command = get_string_field(params, "command", NULL);
    cJSON *argv_arr = cJSON_GetObjectItem(params, "argv");

    if (!command || !cJSON_IsArray(argv_arr)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "command and argv array required",
                           id);
        return;
    }

    size_t argc = (size_t)cJSON_GetArraySize(argv_arr);

    char **argv = AIRY_CALLOC(argc + 2, sizeof(char *));
    if (!argv) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Out of memory", id);
        return;
    }
    argv[0] = (char *)command;
    for (size_t i = 0; i < argc; i++) {
        cJSON *item = cJSON_GetArrayItem(argv_arr, (int)i);
        if (cJSON_IsString(item))
            argv[i + 1] = item->valuestring;
    }

    cupolas_execute_command_params_t req = {.command = command, .argv = argv};
    cupolas_execute_command_result_t res = {0};

    int ret = cupolas_service_execute_command(g_service, &req, &res);
    AIRY_FREE(argv);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Command execution failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "exit_code", res.exit_code);
    cJSON_AddStringToObject(result, "stdout", res.stdout_buf ? res.stdout_buf : "");
    cJSON_AddStringToObject(result, "stderr", res.stderr_buf ? res.stderr_buf : "");
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    cupolas_execute_command_result_free(&res);
}

static void handle_add_rule(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *agent_id = get_string_field(params, "agent_id", NULL);
    const char *action = get_string_field(params, "action", NULL);
    const char *resource = get_string_field(params, "resource", NULL);
    if (!resource) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "resource required", id);
        return;
    }

    int allow = 0;
    cJSON *allow_json = cJSON_GetObjectItem(params, "allow");
    if (cJSON_IsBool(allow_json))
        allow = cJSON_IsTrue(allow_json) ? 1 : 0;
    else if (cJSON_IsNumber(allow_json))
        allow = allow_json->valueint ? 1 : 0;
    int priority = get_int_field(params, "priority", 0);

    cupolas_add_rule_params_t req = {.agent_id = agent_id,
                                     .action = action,
                                     .resource = resource,
                                     .allow = allow,
                                     .priority = priority};
    cupolas_add_rule_result_t res = {0};

    int ret = cupolas_service_add_rule(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Add rule failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "added", res.added ? true : false);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_audit_flush(cJSON *params __attribute__((unused)), int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    int ret = cupolas_service_audit_flush(g_service);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Audit flush failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "flushed", true);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_health_check(int id, airy_sock_t client_fd)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "cupolas_d");
    cJSON_AddBoolToObject(result, "healthy", g_service != NULL);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_get_stats(int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    char *stats_json = cupolas_service_get_stats_json(g_service);
    if (!stats_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Failed to collect stats", id);
        return;
    }

    cJSON *result = cJSON_Parse(stats_json);
    AIRY_FREE(stats_json);
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Stats serialization failed", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_vault_store(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *cred_id = get_string_field(params, "cred_id", NULL);
    const char *data_hex = get_string_field(params, "data", NULL);
    const char *agent_id = get_string_field(params, "agent_id", NULL);
    int cred_type = get_int_field(params, "type", 0);

    if (!cred_id || !data_hex) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "cred_id and data (hex) required",
                           id);
        return;
    }

    size_t data_len = 0;
    uint8_t *data = hex_decode(data_hex, &data_len);
    if (!data) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "data must be even-length hex", id);
        return;
    }

    cupolas_vault_store_params_t req = {.cred_id = cred_id,
                                        .cred_type = cred_type,
                                        .data = data,
                                        .data_len = data_len,
                                        .agent_id = agent_id};
    cupolas_vault_store_result_t res = {0};

    int ret = cupolas_service_vault_store(g_service, &req, &res);
    AIRY_FREE(data);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Vault store failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "stored", res.stored ? true : false);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_vault_retrieve(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *cred_id = get_string_field(params, "cred_id", NULL);
    const char *agent_id = get_string_field(params, "agent_id", NULL);
    if (!cred_id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "cred_id required", id);
        return;
    }

    cupolas_vault_retrieve_params_t req = {.cred_id = cred_id, .agent_id = agent_id};
    cupolas_vault_retrieve_result_t res = {0};

    int ret = cupolas_service_vault_retrieve(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR,
                           "Vault retrieve failed (not found or access denied)", id);
        cupolas_vault_retrieve_result_free(&res);
        return;
    }

    char *data_hex = hex_encode(res.data, res.data_len);
    size_t data_len = res.data_len;
    cupolas_vault_retrieve_result_free(&res);
    if (!data_hex) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Data encoding failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "data", data_hex);
    cJSON_AddNumberToObject(result, "data_len", (double)data_len);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
    AIRY_FREE(data_hex);
}

static void handle_vault_delete(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *cred_id = get_string_field(params, "cred_id", NULL);
    const char *agent_id = get_string_field(params, "agent_id", NULL);
    if (!cred_id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "cred_id required", id);
        return;
    }

    cupolas_vault_delete_params_t req = {.cred_id = cred_id, .agent_id = agent_id};
    cupolas_vault_delete_result_t res = {0};

    int ret = cupolas_service_vault_delete(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Vault delete failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "deleted", res.deleted ? true : false);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_vault_list(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    int cred_type = get_int_field(params, "type", 0);
    char *list_json = cupolas_service_vault_list_json(g_service, cred_type);
    if (!list_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Vault list failed", id);
        return;
    }

    cJSON *result = cJSON_Parse(list_json);
    AIRY_FREE(list_json);
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Vault list serialization failed",
                           id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_vault_rotate(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *cred_group = get_string_field(params, "cred_group", NULL);
    if (!cred_group) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "cred_group required", id);
        return;
    }
    int strategy = get_int_field(params, "strategy", (int)CUPOLAS_VAULT_ROTATE_ROUND_ROBIN);

    cupolas_vault_rotate_params_t req;
    __builtin_memset(&req, 0, sizeof(req));
    req.cred_group = cred_group;
    req.strategy = strategy;

    cupolas_vault_rotate_result_t res = {0};
    int ret = cupolas_service_vault_rotate(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd,
                           ret == AIRY_ERR_INVALID_PARAM ? JSONRPC_INVALID_PARAMS :
                                                           JSONRPC_INTERNAL_ERROR,
                           ret == AIRY_ERR_INVALID_PARAM ? "invalid cred_group/strategy" :
                                                           "vault rotate failed",
                           id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "selected_id", res.selected_id ? res.selected_id : "");
    AIRY_FREE(res.selected_id);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_net_add_rule(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *rule_id = get_string_field(params, "rule_id", NULL);
    if (!rule_id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "rule_id required", id);
        return;
    }

    cupolas_net_add_rule_params_t req;
    __builtin_memset(&req, 0, sizeof(req));
    req.rule_id = rule_id;
    req.src_ip = get_string_field(params, "src_ip", NULL);
    req.dst_ip = get_string_field(params, "dst_ip", NULL);
    req.src_port = get_string_field(params, "src_port", NULL);
    req.dst_port = get_string_field(params, "dst_port", NULL);
    req.protocol = get_int_field(params, "protocol", 0);
    req.direction = get_int_field(params, "direction", 0);
    req.action = get_int_field(params, "action", 0);
    req.priority = get_int_field(params, "priority", 0);
    req.description = get_string_field(params, "description", NULL);

    cupolas_net_add_rule_result_t res = {0};
    int ret = cupolas_service_net_add_rule(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd,
                           ret == AIRY_ERR_INVALID_PARAM ? JSONRPC_INVALID_PARAMS :
                                                           JSONRPC_INTERNAL_ERROR,
                           "Net add rule failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "added", res.added ? true : false);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_net_check_access(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *host = get_string_field(params, "host", NULL);
    int port = get_int_field(params, "port", 0);
    int protocol = get_int_field(params, "protocol", 0);
    const char *direction = get_string_field(params, "direction", NULL);
    if (!host || port < 0 || port > 65535) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "host required, port must be 0-65535",
                           id);
        return;
    }

    cupolas_net_check_access_params_t req = {.host = host,
                                             .port = (uint16_t)port,
                                             .protocol = protocol,
                                             .direction = direction};
    cupolas_net_check_access_result_t res = {0};

    int ret = cupolas_service_net_check_access(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {

        if (ret != AIRY_ERR_PERMISSION_DENIED) {
            JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Net check access failed", id);
            return;
        }
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "allowed", res.allowed ? true : false);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_net_get_stats(int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    char *stats_json = cupolas_service_net_get_stats_json(g_service);
    if (!stats_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Net stats collection failed", id);
        return;
    }

    cJSON *result = cJSON_Parse(stats_json);
    AIRY_FREE(stats_json);
    if (!result) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Net stats serialization failed", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_entitlements_load(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *yaml_path = get_string_field(params, "yaml_path", NULL);
    if (!yaml_path) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "yaml_path required", id);
        return;
    }

    cupolas_entitlements_load_params_t req = {.yaml_path = yaml_path};
    cupolas_entitlements_load_result_t res = {0};

    int ret = cupolas_service_entitlements_load(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Entitlements load failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "loaded", res.loaded ? true : false);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_entitlements_check(cJSON *params, int id, airy_sock_t client_fd)
{
    if (!g_service) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Cupolas service not ready", id);
        return;
    }

    const char *kind = get_string_field(params, "kind", NULL);
    const char *param1 = get_string_field(params, "param1", NULL);
    const char *param2 = get_string_field(params, "param2", NULL);
    if (!kind || !param1) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "kind and param1 required", id);
        return;
    }

    cupolas_entitlements_check_params_t req = {.kind = kind, .param1 = param1, .param2 = param2};
    cupolas_entitlements_check_result_t res = {0};

    int ret = cupolas_service_entitlements_check(g_service, &req, &res);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd,
                           ret == AIRY_ERR_INVALID_PARAM ? JSONRPC_INVALID_PARAMS :
                                                           JSONRPC_INTERNAL_ERROR,
                           "Entitlements check failed", id);
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "allowed", res.allowed ? true : false);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

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
    method_dispatcher_register(g_dispatcher_cupolas_d, "check_permission",
                               on_check_permission_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "sanitize", on_sanitize_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "execute_command", on_execute_command_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "add_rule", on_add_rule_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "audit_flush", on_audit_flush_method, NULL);
    /* L2 protocol standard methods (02-l2-service-protocol.md:
     * cupolas.health_check / cupolas.get_stats / cupolas.shutdown) */
    method_dispatcher_register(g_dispatcher_cupolas_d, "health_check", on_health_check_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "get_stats", on_get_stats_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "shutdown", on_shutdown_method_cupolas_d,
                               NULL);

    method_dispatcher_register(g_dispatcher_cupolas_d, "vault_store", on_vault_store_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "vault_retrieve", on_vault_retrieve_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "vault_delete", on_vault_delete_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "vault_list", on_vault_list_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "vault_rotate", on_vault_rotate_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "net_add_rule", on_net_add_rule_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "net_check_access",
                               on_net_check_access_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "net_get_stats", on_net_get_stats_method,
                               NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "entitlements_load",
                               on_entitlements_load_method, NULL);
    method_dispatcher_register(g_dispatcher_cupolas_d, "entitlements_check",
                               on_entitlements_check_method, NULL);
    SVC_LOG_INFO("Registered %d RPC methods (cupolas.* namespace)", 18);

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
                            destroy_service, &g_running_lock_cupolas_d);
    free_daemon_config();

    SVC_LOG_INFO("Cupolas service stopped");
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
