// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file a2a_rpc.c
 * @brief a2a.* RPC 方法域：Agent Card 注册 / 发现、任务生命周期、
 *        消息投递、计数与健康面。
 *
 * 由 main.c 按单一职责拆分（0.1.19 gen5 装配）：method_fn 薄壳 m_* 把
 * 机制层注入的 &client_fd 翻译为协议无关的 handler（cJSON*, int,
 * airy_sock_t）。L2 协议别名 send → send_message、receive → get_task
 * （02-l2-service-protocol.md）。
 *
 * 结果成型下沉到适配器（route 层）后，业务 handler 只做薄透传：成功即把
 * 适配器结果 JSON 直接回给客户端，失败按既有错误码/文案回错，本层不再
 * 重建结果对象，避免双重成型。
 */

#include "airy_memory.h"
#include "error.h"
#include "a2a_d_internal.h"
#include "svc_a2a_d.h"

#include "jsonrpc_helpers.h"
#include "param_validator.h"
#include "svc_logger.h"

#include <time.h>

static void handle_register_agent(cJSON *params, int id, airy_sock_t fd);
static void handle_unregister_agent(cJSON *params, int id, airy_sock_t fd);
static void handle_discover(cJSON *params, int id, airy_sock_t fd);
static void handle_create_task(cJSON *params, int id, airy_sock_t fd);
static void handle_update_task(cJSON *params, int id, airy_sock_t fd);
static void handle_cancel_task(cJSON *params, int id, airy_sock_t fd);
static void handle_get_task(cJSON *params, int id, airy_sock_t fd);
static void handle_send_message(cJSON *params, int id, airy_sock_t fd);
static void handle_count(int id, airy_sock_t fd);
static void handle_health_check(int id, airy_sock_t fd);
static void handle_get_stats(int id, airy_sock_t fd);

void m_register_agent(cJSON *params, int id, void *user_data)
{
    handle_register_agent(params, id, *(airy_sock_t *)user_data);
}

void m_unregister_agent(cJSON *params, int id, void *user_data)
{
    handle_unregister_agent(params, id, *(airy_sock_t *)user_data);
}

void m_discover_agents(cJSON *params, int id, void *user_data)
{
    handle_discover(params, id, *(airy_sock_t *)user_data);
}

void m_create_task(cJSON *params, int id, void *user_data)
{
    handle_create_task(params, id, *(airy_sock_t *)user_data);
}

void m_update_task(cJSON *params, int id, void *user_data)
{
    handle_update_task(params, id, *(airy_sock_t *)user_data);
}

void m_cancel_task(cJSON *params, int id, void *user_data)
{
    handle_cancel_task(params, id, *(airy_sock_t *)user_data);
}

void m_get_task(cJSON *params, int id, void *user_data)
{
    handle_get_task(params, id, *(airy_sock_t *)user_data);
}

void m_send_message(cJSON *params, int id, void *user_data)
{
    handle_send_message(params, id, *(airy_sock_t *)user_data);
}

/* L2 协议别名：a2a.send == send_message；a2a.receive == get_task。 */
void m_send(cJSON *params, int id, void *user_data)
{
    handle_send_message(params, id, *(airy_sock_t *)user_data);
}

void m_receive(cJSON *params, int id, void *user_data)
{
    handle_get_task(params, id, *(airy_sock_t *)user_data);
}

void m_count(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_count(id, *(airy_sock_t *)user_data);
}

void m_health_check(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_health_check(id, *(airy_sock_t *)user_data);
}

void m_get_stats(cJSON *params, int id, void *user_data)
{
    (void)params;
    handle_get_stats(id, *(airy_sock_t *)user_data);
}

static char *a2a_cjson_to_string(cJSON *obj)
{
    if (!obj)
        return NULL;
    return cJSON_PrintUnformatted(obj);
}

static void send_result(airy_sock_t fd, int id, char *result_json)
{
    if (!result_json) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Result serialize failed", id);
        return;
    }
    cJSON *result = cJSON_Parse(result_json);
    if (!result) {
        JSONRPC_SEND_ERROR(fd, JSONRPC_INTERNAL_ERROR, "Result serialize failed", id);
        return;
    }
    JSONRPC_SEND_SUCCESS(fd, result, id);
}

static void handle_register_agent(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_id = cJSON_GetObjectItem(params, "id");
    cJSON *name = cJSON_GetObjectItem(params, "name");

    if (!agent_id || !cJSON_IsString(agent_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing id string", id);
        return;
    }
    if (!name || !cJSON_IsString(name)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing name string", id);
        return;
    }

    char *card_json = a2a_cjson_to_string(params);
    if (!card_json) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Card serialize failed", id);
        return;
    }

    char *result_json = NULL;
    int ret = a2a_service_register_agent(g_service, card_json, &result_json);
    AIRY_FREE(card_json);

    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Agent register failed", id);
        SVC_LOG_ERROR("a2a.register_agent failed: error=%d", ret);
        a2a_result_free(result_json);
        return;
    }

    send_result(client_fd, id, result_json);
    a2a_result_free(result_json);
}

static void handle_unregister_agent(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_id = cJSON_GetObjectItem(params, "agent_id");
    if (!agent_id || !cJSON_IsString(agent_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }

    char *result_json = NULL;
    int ret = a2a_service_unregister_agent(g_service, agent_id->valuestring, &result_json);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Agent not found", id);
        a2a_result_free(result_json);
        return;
    }

    send_result(client_fd, id, result_json);
    a2a_result_free(result_json);
}

static void handle_discover(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *capability = cJSON_GetObjectItem(params, "capability");
    cJSON *skill = cJSON_GetObjectItem(params, "skill");

    const char *cap_str =
        (capability && cJSON_IsString(capability)) ? capability->valuestring : NULL;
    const char *skill_str = (skill && cJSON_IsString(skill)) ? skill->valuestring : NULL;

    char *result_json = NULL;
    int ret = a2a_service_discover_agents(g_service, cap_str, skill_str, &result_json);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Discover failed", id);
        a2a_result_free(result_json);
        return;
    }

    send_result(client_fd, id, result_json);
    a2a_result_free(result_json);
}

static void handle_create_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *agent_id = cJSON_GetObjectItem(params, "agent_id");
    cJSON *description = cJSON_GetObjectItem(params, "description");
    cJSON *input = cJSON_GetObjectItem(params, "input");

    if (!agent_id || !cJSON_IsString(agent_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing agent_id", id);
        return;
    }
    if (!description || !cJSON_IsString(description)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing description", id);
        return;
    }

    const char *input_str = (input && cJSON_IsString(input)) ? input->valuestring : NULL;

    char *result_json = NULL;
    int ret = a2a_service_create_task(g_service, agent_id->valuestring, description->valuestring,
                                      input_str, &result_json);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Task create failed", id);
        SVC_LOG_ERROR("a2a.create_task failed: error=%d", ret);
        a2a_result_free(result_json);
        return;
    }

    send_result(client_fd, id, result_json);
    a2a_result_free(result_json);
}

static void handle_update_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *task_id = cJSON_GetObjectItem(params, "task_id");
    cJSON *state = cJSON_GetObjectItem(params, "state");
    cJSON *output = cJSON_GetObjectItem(params, "output");
    cJSON *progress = cJSON_GetObjectItem(params, "progress");

    if (!task_id || !cJSON_IsString(task_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing task_id", id);
        return;
    }
    if (!state || !cJSON_IsNumber(state)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing state", id);
        return;
    }

    const char *output_str = (output && cJSON_IsString(output)) ? output->valuestring : NULL;
    double prog = (progress && cJSON_IsNumber(progress)) ? progress->valuedouble : 0.0;

    char *result_json = NULL;
    int ret = a2a_service_update_task(g_service, task_id->valuestring, state->valueint, output_str,
                                      prog, &result_json);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Task not found", id);
        a2a_result_free(result_json);
        return;
    }

    send_result(client_fd, id, result_json);
    a2a_result_free(result_json);
}

static void handle_cancel_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *task_id = cJSON_GetObjectItem(params, "task_id");
    cJSON *reason = cJSON_GetObjectItem(params, "reason");

    if (!task_id || !cJSON_IsString(task_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing task_id", id);
        return;
    }

    const char *reason_str = (reason && cJSON_IsString(reason)) ? reason->valuestring : NULL;

    char *result_json = NULL;
    int ret = a2a_service_cancel_task(g_service, task_id->valuestring, reason_str, &result_json);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Task not found", id);
        a2a_result_free(result_json);
        return;
    }

    send_result(client_fd, id, result_json);
    a2a_result_free(result_json);
}

static void handle_get_task(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *task_id = cJSON_GetObjectItem(params, "task_id");
    if (!task_id || !cJSON_IsString(task_id)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing task_id", id);
        return;
    }

    char *result_json = NULL;
    int ret = a2a_service_get_task(g_service, task_id->valuestring, &result_json);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_METHOD_NOT_FOUND, "Task not found", id);
        a2a_result_free(result_json);
        return;
    }

    send_result(client_fd, id, result_json);
    a2a_result_free(result_json);
}

static void handle_send_message(cJSON *params, int id, airy_sock_t client_fd)
{
    cJSON *target = cJSON_GetObjectItem(params, "target_agent_id");
    cJSON *role = cJSON_GetObjectItem(params, "role");
    cJSON *content = cJSON_GetObjectItem(params, "content");

    if (!target || !cJSON_IsString(target)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing target_agent_id", id);
        return;
    }
    if (!role || !cJSON_IsString(role)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing role", id);
        return;
    }
    if (!content || !cJSON_IsString(content)) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INVALID_PARAMS, "Missing content", id);
        return;
    }

    char *result_json = NULL;
    int ret = a2a_service_send_message(g_service, target->valuestring, role->valuestring,
                                       content->valuestring, &result_json);
    if (ret != AIRY_SUCCESS) {
        JSONRPC_SEND_ERROR(client_fd, JSONRPC_INTERNAL_ERROR, "Send message failed", id);
        SVC_LOG_ERROR("a2a.send_message failed: error=%d", ret);
        a2a_result_free(result_json);
        return;
    }

    send_result(client_fd, id, result_json);
    a2a_result_free(result_json);
}

static void handle_count(int id, airy_sock_t client_fd)
{
    size_t agent_count = 0;
    size_t task_count = 0;
    if (g_service)
        (void)a2a_service_stats(g_service, &agent_count, &task_count);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "agent_count", (double)agent_count);
    cJSON_AddNumberToObject(result, "task_count", (double)task_count);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_health_check(int id, airy_sock_t client_fd)
{
    bool healthy = g_service != NULL;
    size_t agent_count = 0;
    size_t task_count = 0;
    if (healthy)
        healthy = a2a_service_stats(g_service, &agent_count, &task_count) == AIRY_SUCCESS;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "service", "a2a_d");
    cJSON_AddBoolToObject(result, "healthy", healthy);
    cJSON_AddNumberToObject(result, "agent_count", (double)agent_count);
    cJSON_AddNumberToObject(result, "task_count", (double)task_count);
    cJSON_AddNumberToObject(result, "timestamp", (double)(uint64_t)time(NULL) * 1000);

    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}

static void handle_get_stats(int id, airy_sock_t client_fd)
{
    size_t agent_count = 0;
    size_t task_count = 0;
    if (g_service)
        (void)a2a_service_stats(g_service, &agent_count, &task_count);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "daemon", "a2a_d");
    cJSON_AddNumberToObject(result, "agents", (double)agent_count);
    cJSON_AddNumberToObject(result, "tasks", (double)task_count);
    JSONRPC_SEND_SUCCESS(client_fd, result, id);
}
