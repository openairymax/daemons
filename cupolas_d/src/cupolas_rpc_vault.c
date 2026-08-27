// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file cupolas_rpc_vault.c
 * @brief cupolas.* vault RPC 方法：凭据存取 / 删除 / 列表 / 轮换
 *        （含 hex 编解码工具）。
 *
 * 由 main.c 按单一职责拆分（2026-08-27）。
 */

#include "airy_memory.h"
#include "error.h"
#include "cupolas_d_internal.h"
#include "cupolas_vault.h"

#include "daemon_main.h"
#include "param_validator.h"
#include "svc_logger.h"

#include <string.h>

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

static void handle_vault_store(cJSON *params, int id, airy_sock_t fd);
static void handle_vault_retrieve(cJSON *params, int id, airy_sock_t fd);
static void handle_vault_delete(cJSON *params, int id, airy_sock_t fd);
static void handle_vault_list(cJSON *params, int id, airy_sock_t fd);
static void handle_vault_rotate(cJSON *params, int id, airy_sock_t fd);

/* cupolas.vault_store */
void on_vault_store_method(cJSON *params, int id, void *user_data)
{
    handle_vault_store(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.vault_retrieve */
void on_vault_retrieve_method(cJSON *params, int id, void *user_data)
{
    handle_vault_retrieve(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.vault_delete */
void on_vault_delete_method(cJSON *params, int id, void *user_data)
{
    handle_vault_delete(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.vault_list */
void on_vault_list_method(cJSON *params, int id, void *user_data)
{
    handle_vault_list(params, id, *(airy_sock_t *)user_data);
}

/* cupolas.vault_rotate */
void on_vault_rotate_method(cJSON *params, int id, void *user_data)
{
    handle_vault_rotate(params, id, *(airy_sock_t *)user_data);
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
