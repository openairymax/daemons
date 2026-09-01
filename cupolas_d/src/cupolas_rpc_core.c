// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file cupolas_rpc_core.c
 * @brief cupolas.* 核心 RPC 方法：权限裁决 / 输入净化 / 命令执行 /
 *        规则管理 / 审计 / 健康检查 / 服务统计。
 *
 * 由 main.c 按单一职责拆分（2026-08-27）：vault 相关见 cupolas_rpc_vault.c，
 * 网络与 entitlements 见 cupolas_rpc_net_entitlements.c。
 */

#include "airy_memory.h"
#include "error.h"
#include "cupolas_d_internal.h"

#include "daemon_main.h"
#include "param_validator.h"
#include "svc_logger.h"

#include "dynamic_policy_engine.h"

#include <time.h>

static void handle_check_permission(cJSON *params, int id, airy_sock_t fd);
static void handle_sanitize(cJSON *params, int id, airy_sock_t fd);
static void handle_execute_command(cJSON *params, int id, airy_sock_t fd);
static void handle_add_rule(cJSON *params, int id, airy_sock_t fd);
static void handle_audit_flush(cJSON *params, int id, airy_sock_t fd);
static void handle_get_stats(int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);

void on_check_permission_method(cJSON *params, int id, void *user_data)
{
    handle_check_permission(params, id, *(airy_sock_t *)user_data);
}

void on_sanitize_method(cJSON *params, int id, void *user_data)
{
    handle_sanitize(params, id, *(airy_sock_t *)user_data);
}

void on_execute_command_method(cJSON *params, int id, void *user_data)
{
    handle_execute_command(params, id, *(airy_sock_t *)user_data);
}

void on_add_rule_method(cJSON *params, int id, void *user_data)
{
    handle_add_rule(params, id, *(airy_sock_t *)user_data);
}

void on_audit_flush_method(cJSON *params, int id, void *user_data)
{
    handle_audit_flush(params, id, *(airy_sock_t *)user_data);
}

void on_get_stats_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

void on_health_check_method(cJSON *params __attribute__((unused)), int id, void *user_data)
{
    handle_health_check(id, *(airy_sock_t *)user_data);
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
    /* M2-S5 (0.1.9 §3.2 PEP)：权威 epoch 随裁定返回——PEP 缓存以
     * 该值对齐失效键（epoch 是策略版本 SSoT，见 policy_status）。 */
    if (g_dpolicy)
        cJSON_AddNumberToObject(result, "epoch",
                                (double)dpolicy_engine_get_epoch(g_dpolicy));
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
