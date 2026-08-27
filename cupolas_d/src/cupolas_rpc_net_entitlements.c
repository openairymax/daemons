// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file cupolas_rpc_net_entitlements.c
 * @brief cupolas.* 网络规则与 entitlements RPC 方法：net_add_rule /
 *        net_check_access / net_get_stats / entitlements_load /
 *        entitlements_check。
 *
 * 由 main.c 按单一职责拆分（2026-08-27）。
 */

#include "airy_memory.h"
#include "error.h"
#include "cupolas_d_internal.h"

#include "daemon_main.h"
#include "param_validator.h"
#include "svc_logger.h"

static void handle_net_add_rule(cJSON *params, int id, airy_sock_t fd);
static void handle_net_check_access(cJSON *params, int id, airy_sock_t fd);
static void handle_net_get_stats(int id, airy_sock_t fd);
static void handle_entitlements_load(cJSON *params, int id, airy_sock_t fd);
static void handle_entitlements_check(cJSON *params, int id, airy_sock_t fd);

/* cupolas.net_add_rule */
void on_net_add_rule_method(cJSON *params, int id, void *user_data)
{
    handle_net_add_rule(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.net_check_access */
void on_net_check_access_method(cJSON *params, int id, void *user_data)
{
    handle_net_check_access(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.net_get_stats */
void on_net_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_net_get_stats(id, *(airy_sock_t *)user_data);
}

/* cupolas.entitlements_load */
void on_entitlements_load_method(cJSON *params, int id, void *user_data)
{
    handle_entitlements_load(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.entitlements_check */
void on_entitlements_check_method(cJSON *params, int id, void *user_data)
{
    handle_entitlements_check(params, id, *(airy_sock_t *)user_data);
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
