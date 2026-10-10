// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file skill_rpc.c
 * @brief skill.* RPC 服务面：技能安装 / 列举 / 执行 / 卸载。
 *
 * atoms/syscall 技能机制（airy_sys_skill_*，注册表 + 七类内建执行器 +
 * custom 执行器注册表）此前只有 syscall 表入口，无 daemon 服务面暴露。
 * 本模块把该机制登记到 tool 命名空间（skill.install/list/execute/
 * uninstall），闭合「内核机制只被 daemon 服务面访问」的服务通路；
 * 生态技能资产（ecosystem/skills，含 C 插件与 Python 实现）经
 * file: URL 安装到注册表后由本面执行。
 */

#include "airy_memory.h"
#include "error.h"
#include "jsonrpc_helpers.h"
#include "method_dispatcher.h"
#include "param_validator.h"
#include "svc_logger.h"
#include "syscalls.h"

#include <cjson/cJSON.h>

#include <string.h>

static void srpc_install(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    const char *url = get_string_field(params, "url", NULL);
    if (!url || !*url) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing url", id);
        return;
    }

    char *skill_id = NULL;
    airy_err_t err = airy_sys_skill_install(url, &skill_id);
    if (err != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "skill install failed", id);
        return;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "skill_id", skill_id ? skill_id : "");
    JSONRPC_SEND_SUCCESS(client_fd, obj, id);
    AIRY_FREE(skill_id);
}

static void srpc_list(cJSON *params, int id, void *user_data)
{
    (void)params;
    airy_sock_t client_fd = *(airy_sock_t *)user_data;

    char **skills = NULL;
    size_t count = 0;
    airy_err_t err = airy_sys_skill_list(&skills, &count);
    if (err != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "skill list failed", id);
        return;
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(skills[i]));
        AIRY_FREE(skills[i]);
    }
    AIRY_FREE(skills);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddItemToObject(obj, "skills", arr);
    cJSON_AddNumberToObject(obj, "count", (double)count);
    JSONRPC_SEND_SUCCESS(client_fd, obj, id);
}

static void srpc_execute(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    const char *skill_id = get_string_field(params, "skill_id", NULL);
    const char *input = get_string_field(params, "input", NULL);
    if (!skill_id || !*skill_id || !input) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS,
                           "Missing skill_id or input", id);
        return;
    }

    char *output = NULL;
    airy_err_t err = airy_sys_skill_execute(skill_id, input, &output);
    if (err == AIRY_ENOENT) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Skill not found", id);
        return;
    }
    if (err != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "skill execute failed", id);
        return;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "output", output ? output : "");
    JSONRPC_SEND_SUCCESS(client_fd, obj, id);
    AIRY_FREE(output);
}

static void srpc_uninstall(cJSON *params, int id, void *user_data)
{
    airy_sock_t client_fd = *(airy_sock_t *)user_data;
    const char *skill_id = get_string_field(params, "skill_id", NULL);
    if (!skill_id || !*skill_id) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing skill_id", id);
        return;
    }

    airy_err_t err = airy_sys_skill_uninstall(skill_id);
    if (err == AIRY_ENOENT) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Skill not found", id);
        return;
    }
    if (err != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "skill uninstall failed", id);
        return;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    JSONRPC_SEND_SUCCESS(client_fd, obj, id);
}

void skill_rpc_register(void *dispatcher)
{
    method_dispatcher_t *d = (method_dispatcher_t *)dispatcher;
    method_dispatcher_register(d, "skill.install", srpc_install, NULL);
    method_dispatcher_register(d, "skill.list", srpc_list, NULL);
    method_dispatcher_register(d, "skill.execute", srpc_execute, NULL);
    method_dispatcher_register(d, "skill.uninstall", srpc_uninstall, NULL);
}
