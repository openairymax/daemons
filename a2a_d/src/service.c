#include "airy_memory.h"
#include "error.h"
/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file service.c
 * @brief A2A 服务实现：封装 a2a_v03_adapter 库
 *
 * 将 a2a_v03_adapter 库的 C API 适配为面向 a2a_d 守护进程的服务模块。
 * 守护进程 a2a_d 持有 a2a_service_t 实例并通过 Unix socket 暴露
 * a2a.* 命名空间方法（智能体注册/发现、任务生命周期、消息传递）。
 *
 * 设计要点：
 * - 持有 a2a_v03_context_t*，所有操作委托给 adapter 库
 * - 线程安全：所有公共接口持锁
 * - JSON 序列化：使用 cJSON 构建，返回 AIRY_STRDUP'd 字符串
 * - 内存所有权遵循 adapter 库契约（见各函数注释）
 */

#include "service.h"

#include "svc_logger.h"

#include <a2a_v03_adapter.h>
#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define A2A_DEFAULT_MAX_AGENTS 256
#define A2A_DEFAULT_MAX_TASKS 4096

/* ==================== JSON 序列化辅助 ==================== */

static void a2a_add_str_item(cJSON *obj, const char *key, const char *val)
{
    if (val)
        cJSON_AddStringToObject(obj, key, val);
    else
        cJSON_AddNullToObject(obj, key);
}

/* 将 Agent Card 序列化为 cJSON 对象（不接管所有权） */
static cJSON *a2a_card_to_json(const a2a_agent_card_t *card)
{
    if (!card)
        return NULL;

    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;

    a2a_add_str_item(obj, "id", card->id);
    a2a_add_str_item(obj, "name", card->name);
    a2a_add_str_item(obj, "description", card->description);
    a2a_add_str_item(obj, "url", card->url);
    a2a_add_str_item(obj, "version", card->version);
    cJSON_AddNumberToObject(obj, "protocol_version", (double)card->protocol_version);
    cJSON_AddNumberToObject(obj, "capabilities", (double)(int)card->capabilities);
    cJSON_AddBoolToObject(obj, "available", card->available);

    return obj;
}

/* 将 Task 序列化为 cJSON 对象 */
static cJSON *a2a_task_to_json(const a2a_task_t *task)
{
    if (!task)
        return NULL;

    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;

    a2a_add_str_item(obj, "id", task->id);
    a2a_add_str_item(obj, "session_id", task->session_id);
    a2a_add_str_item(obj, "agent_id", task->agent_id);
    cJSON_AddNumberToObject(obj, "state", (double)task->state);
    a2a_add_str_item(obj, "description", task->description);
    a2a_add_str_item(obj, "input_json", task->input_json);
    a2a_add_str_item(obj, "output_json", task->output_json);
    cJSON_AddNumberToObject(obj, "progress", task->progress);
    cJSON_AddNumberToObject(obj, "created_at", (double)task->created_at);
    cJSON_AddNumberToObject(obj, "updated_at", (double)task->updated_at);
    a2a_add_str_item(obj, "error_message", task->error_message);

    return obj;
}

/* 将 Message 序列化为 cJSON 对象 */
static cJSON *a2a_message_to_json(const a2a_message_t *msg)
{
    if (!msg)
        return NULL;

    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;

    a2a_add_str_item(obj, "role", msg->role);
    cJSON_AddNumberToObject(obj, "type", (double)msg->type);
    a2a_add_str_item(obj, "content_json", msg->content_json);
    a2a_add_str_item(obj, "mime_type", msg->mime_type);

    return obj;
}

/* ==================== 生命周期 ==================== */

a2a_service_t *a2a_service_create(size_t max_agents, size_t max_tasks)
{
    if (max_agents == 0)
        max_agents = A2A_DEFAULT_MAX_AGENTS;
    if (max_tasks == 0)
        max_tasks = A2A_DEFAULT_MAX_TASKS;

    a2a_service_t *svc = (a2a_service_t *)AIRY_CALLOC(1, sizeof(a2a_service_t));
    if (!svc)
        return NULL;

    a2a_v03_config_t config = a2a_v03_config_default();
    config.max_agents = max_agents;
    config.max_tasks = max_tasks;

    svc->ctx = a2a_v03_context_create(&config);
    if (!svc->ctx) {
        AIRY_FREE(svc);
        SVC_LOG_ERROR("A2A context creation failed");
        return NULL;
    }

    svc->max_agents = max_agents;
    svc->max_tasks = max_tasks;
    airy_mtx_init(&svc->lock);
    svc->initialized = 1;
    SVC_LOG_INFO("A2A service created (max_agents=%zu, max_tasks=%zu)",
                 max_agents, max_tasks);
    return svc;
}

void a2a_service_destroy(a2a_service_t *svc)
{
    if (!svc)
        return;

    airy_mtx_lock(&svc->lock);
    if (svc->ctx) {
        a2a_v03_context_destroy(svc->ctx);
        svc->ctx = NULL;
    }
    svc->initialized = 0;
    svc->max_agents = 0;
    svc->max_tasks = 0;
    airy_mtx_unlock(&svc->lock);
    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

/* ==================== Agent Card 管理 ==================== */

int a2a_service_register_agent(a2a_service_t *svc, const char *card_json)
{
    if (!svc || !svc->initialized || !card_json)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *root = cJSON_Parse(card_json);
    if (!root) {
        SVC_LOG_WARN("A2A register: invalid card JSON");
        return AIRY_ERR_INVALID_PARAM;
    }

    a2a_agent_card_t card;
    __builtin_memset(&card, 0, sizeof(card));

    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *description = cJSON_GetObjectItem(root, "description");
    cJSON *url = cJSON_GetObjectItem(root, "url");
    cJSON *version = cJSON_GetObjectItem(root, "version");
    cJSON *protocol_version = cJSON_GetObjectItem(root, "protocol_version");
    cJSON *capabilities = cJSON_GetObjectItem(root, "capabilities");
    cJSON *available = cJSON_GetObjectItem(root, "available");
    cJSON *skills = cJSON_GetObjectItem(root, "skills");

    /* id 必填 */
    if (!id || !cJSON_IsString(id) || id->valuestring[0] == '\0') {
        cJSON_Delete(root);
        SVC_LOG_WARN("A2A register: missing agent id");
        return AIRY_ERR_INVALID_PARAM;
    }

    card.id = AIRY_STRDUP(id->valuestring);
    card.name = AIRY_STRDUP(name && cJSON_IsString(name) ? name->valuestring : "Unknown");
    card.description = (description && cJSON_IsString(description))
                           ? AIRY_STRDUP(description->valuestring) : NULL;
    card.url = (url && cJSON_IsString(url)) ? AIRY_STRDUP(url->valuestring) : NULL;
    card.version = (version && cJSON_IsString(version)) ? AIRY_STRDUP(version->valuestring) : NULL;
    card.protocol_version = (protocol_version && cJSON_IsNumber(protocol_version))
                                ? protocol_version->valueint : 3;
    card.capabilities = (a2a_capability_t)((capabilities && cJSON_IsNumber(capabilities))
                                               ? capabilities->valueint : 0);
    card.available = available ? cJSON_IsTrue(available) : true;

    /* skills 数组可选：存在则解析 */
    if (skills && cJSON_IsArray(skills)) {
        size_t skill_count = (size_t)cJSON_GetArraySize(skills);
        if (skill_count > 0) {
            card.skills = (a2a_skill_t *)AIRY_CALLOC(skill_count, sizeof(a2a_skill_t));
            if (card.skills) {
                size_t idx = 0;
                cJSON *skill = NULL;
                cJSON_ArrayForEach(skill, skills) {
                    if (idx >= skill_count)
                        break;
                    cJSON *sname = cJSON_GetObjectItem(skill, "name");
                    cJSON *sdesc = cJSON_GetObjectItem(skill, "description");
                    cJSON *sschema = cJSON_GetObjectItem(skill, "schema_json");
                    card.skills[idx].name = (sname && cJSON_IsString(sname))
                                                ? AIRY_STRDUP(sname->valuestring) : NULL;
                    card.skills[idx].description = (sdesc && cJSON_IsString(sdesc))
                                                       ? AIRY_STRDUP(sdesc->valuestring) : NULL;
                    card.skills[idx].schema_json = (sschema && cJSON_IsString(sschema))
                                                       ? AIRY_STRDUP(sschema->valuestring) : NULL;
                    idx++;
                }
                card.skill_count = idx;
            }
        }
    }

    /* 保存 id 字符串用于日志：root/card 释放后 id->valuestring 不可用 */
    const char *registered_id = card.id ? AIRY_STRDUP(card.id) : NULL;

    airy_mtx_lock(&svc->lock);
    int rc = a2a_v03_register_agent(svc->ctx, &card);
    airy_mtx_unlock(&svc->lock);

    /* adapter 库内部通过 STRNCPY 复制所需字段，调用方释放 card 字段 */
    a2a_agent_card_destroy(&card);
    cJSON_Delete(root);

    if (rc != AIRY_SUCCESS) {
        SVC_LOG_ERROR("A2A register_agent failed: rc=%d", rc);
    } else {
        SVC_LOG_DEBUG("A2A agent registered: id=%s",
                       registered_id ? registered_id : "(null)");
    }
    AIRY_FREE((void *)registered_id);
    return rc;
}

int a2a_service_unregister_agent(a2a_service_t *svc, const char *agent_id)
{
    if (!svc || !svc->initialized || !agent_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    int rc = a2a_v03_unregister_agent(svc->ctx, agent_id);
    airy_mtx_unlock(&svc->lock);

    if (rc != AIRY_SUCCESS)
        SVC_LOG_WARN("A2A unregister_agent failed: id=%s, rc=%d", agent_id, rc);
    else
        SVC_LOG_DEBUG("A2A agent unregistered: id=%s", agent_id);
    return rc;
}

int a2a_service_get_agent_card(a2a_service_t *svc, const char *agent_id,
                                 char **out_card_json)
{
    if (!svc || !svc->initialized || !agent_id || !out_card_json)
        return AIRY_ERR_INVALID_PARAM;

    *out_card_json = NULL;

    airy_mtx_lock(&svc->lock);
    /* 返回 const 静态卡片指针（由 adapter 持有，不可释放） */
    const a2a_agent_card_t *card = a2a_v03_get_agent_card(svc->ctx, agent_id);
    if (!card) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }

    cJSON *obj = a2a_card_to_json(card);
    airy_mtx_unlock(&svc->lock);

    if (!obj)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!str)
        return AIRY_ERR_OUT_OF_MEMORY;

    *out_card_json = AIRY_STRDUP(str);
    AIRY_FREE(str);  /* cJSON_PrintUnformatted 输出，与 agent_d 同释放约定 */
    if (!*out_card_json)
        return AIRY_ERR_OUT_OF_MEMORY;

    return AIRY_SUCCESS;
}

int a2a_service_discover_agents(a2a_service_t *svc, const char *capability,
                                  const char *skill_name,
                                  char **out_results_json, size_t *out_count)
{
    if (!svc || !svc->initialized || !out_results_json || !out_count)
        return AIRY_ERR_INVALID_PARAM;

    *out_results_json = NULL;
    *out_count = 0;

    airy_mtx_lock(&svc->lock);
    a2a_agent_card_t **results = NULL;
    size_t count = 0;
    int rc = a2a_v03_discover_agents(svc->ctx, capability, skill_name,
                                       &results, &count);
    if (rc != AIRY_SUCCESS) {
        airy_mtx_unlock(&svc->lock);
        SVC_LOG_ERROR("A2A discover_agents failed: rc=%d", rc);
        return rc;
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        /* 释放 adapter 分配的结果数组（每项为新建卡片） */
        if (results) {
            for (size_t i = 0; i < count; i++) {
                if (results[i]) {
                    a2a_agent_card_destroy(results[i]);
                    AIRY_FREE(results[i]);
                }
            }
            AIRY_FREE(results);
        }
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < count; i++) {
        cJSON *item = a2a_card_to_json(results[i]);
        if (item)
            cJSON_AddItemToArray(arr, item);
    }
    airy_mtx_unlock(&svc->lock);

    char *str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    /* 释放 adapter 分配的结果数组与卡片 */
    if (results) {
        for (size_t i = 0; i < count; i++) {
            if (results[i]) {
                a2a_agent_card_destroy(results[i]);
                AIRY_FREE(results[i]);
            }
        }
        AIRY_FREE(results);
    }

    if (!str)
        return AIRY_ERR_OUT_OF_MEMORY;

    *out_results_json = AIRY_STRDUP(str);
    AIRY_FREE(str);
    if (!*out_results_json)
        return AIRY_ERR_OUT_OF_MEMORY;

    *out_count = count;
    SVC_LOG_DEBUG("A2A discover: capability=%s, count=%zu",
                  capability ? capability : "(null)", count);
    return AIRY_SUCCESS;
}

/* ==================== Task 生命周期 ==================== */

int a2a_service_create_task(a2a_service_t *svc, const char *agent_id,
                              const char *description, const char *input_json,
                              char **out_task_json)
{
    if (!svc || !svc->initialized || !agent_id || !out_task_json)
        return AIRY_ERR_INVALID_PARAM;

    *out_task_json = NULL;

    airy_mtx_lock(&svc->lock);
    a2a_task_t *task = NULL;
    int rc = a2a_v03_create_task(svc->ctx, agent_id, description, input_json, &task);
    if (rc != AIRY_SUCCESS || !task) {
        airy_mtx_unlock(&svc->lock);
        SVC_LOG_ERROR("A2A create_task failed: rc=%d", rc);
        return rc;
    }

    cJSON *obj = a2a_task_to_json(task);
    airy_mtx_unlock(&svc->lock);

    if (!obj)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!str)
        return AIRY_ERR_OUT_OF_MEMORY;

    *out_task_json = AIRY_STRDUP(str);
    AIRY_FREE(str);
    if (!*out_task_json)
        return AIRY_ERR_OUT_OF_MEMORY;

    SVC_LOG_DEBUG("A2A task created: agent_id=%s", agent_id);
    return AIRY_SUCCESS;
}

int a2a_service_update_task(a2a_service_t *svc, const char *task_id, int state,
                              const char *output_json, double progress)
{
    if (!svc || !svc->initialized || !task_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    int rc = a2a_v03_update_task(svc->ctx, task_id, (a2a_task_state_t)state,
                                   output_json, progress);
    airy_mtx_unlock(&svc->lock);

    if (rc != AIRY_SUCCESS)
        SVC_LOG_WARN("A2A update_task failed: id=%s, rc=%d", task_id, rc);
    else
        SVC_LOG_DEBUG("A2A task updated: id=%s, state=%d", task_id, state);
    return rc;
}

int a2a_service_cancel_task(a2a_service_t *svc, const char *task_id,
                              const char *reason)
{
    if (!svc || !svc->initialized || !task_id)
        return AIRY_ERR_INVALID_PARAM;

    airy_mtx_lock(&svc->lock);
    int rc = a2a_v03_cancel_task(svc->ctx, task_id, reason);
    airy_mtx_unlock(&svc->lock);

    if (rc != AIRY_SUCCESS)
        SVC_LOG_WARN("A2A cancel_task failed: id=%s, rc=%d", task_id, rc);
    else
        SVC_LOG_DEBUG("A2A task canceled: id=%s", task_id);
    return rc;
}

int a2a_service_get_task(a2a_service_t *svc, const char *task_id,
                           char **out_task_json)
{
    if (!svc || !svc->initialized || !task_id || !out_task_json)
        return AIRY_ERR_INVALID_PARAM;

    *out_task_json = NULL;

    airy_mtx_lock(&svc->lock);
    a2a_task_t *task = NULL;
    int rc = a2a_v03_get_task(svc->ctx, task_id, &task);
    if (rc != AIRY_SUCCESS || !task) {
        airy_mtx_unlock(&svc->lock);
        return AIRY_ERR_NOT_FOUND;
    }

    cJSON *obj = a2a_task_to_json(task);
    airy_mtx_unlock(&svc->lock);

    if (!obj)
        return AIRY_ERR_OUT_OF_MEMORY;

    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!str)
        return AIRY_ERR_OUT_OF_MEMORY;

    *out_task_json = AIRY_STRDUP(str);
    AIRY_FREE(str);
    if (!*out_task_json)
        return AIRY_ERR_OUT_OF_MEMORY;

    return AIRY_SUCCESS;
}

/* ==================== 消息传递 ==================== */

int a2a_service_send_message(a2a_service_t *svc, const char *target_agent_id,
                               const char *role, const char *content_json,
                               char **out_response_json,
                               size_t *out_response_count)
{
    if (!svc || !svc->initialized || !target_agent_id || !role || !content_json
        || !out_response_json || !out_response_count)
        return AIRY_ERR_INVALID_PARAM;

    *out_response_json = NULL;
    *out_response_count = 0;

    a2a_message_t message;
    __builtin_memset(&message, 0, sizeof(message));
    message.role = (char *)role;
    message.type = A2A_MSG_TEXT;
    message.content_json = (char *)content_json;

    /* 网络往返在全局锁外执行：不同目标的消息发送可并行，
     * 不阻塞其他 A2A 操作（a2a_v03_send_message 内部经 handler
     * 分发，ctx 为协议上下文，调用本身不依赖本服务共享状态） */
    a2a_message_t *response = NULL;
    size_t response_count = 0;
    int rc = a2a_v03_send_message(svc->ctx, target_agent_id, &message,
                                    &response, &response_count);
    if (rc != AIRY_SUCCESS) {
        SVC_LOG_ERROR("A2A send_message failed: rc=%d", rc);
        return rc;
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        if (response) {
            for (size_t i = 0; i < response_count; i++)
                a2a_message_destroy(&response[i]);
        }
        return AIRY_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < response_count; i++) {
        cJSON *item = a2a_message_to_json(&response[i]);
        if (item)
            cJSON_AddItemToArray(arr, item);
    }

    char *str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    /* 释放 adapter 分配的响应消息：a2a_message_destroy 已释放 struct 本身，
     * 不可再次 AIRY_FREE(response) 否则 double-free */
    if (response) {
        for (size_t i = 0; i < response_count; i++)
            a2a_message_destroy(&response[i]);
    }

    if (!str)
        return AIRY_ERR_OUT_OF_MEMORY;

    *out_response_json = AIRY_STRDUP(str);
    AIRY_FREE(str);
    if (!*out_response_json)
        return AIRY_ERR_OUT_OF_MEMORY;

    *out_response_count = response_count;
    SVC_LOG_DEBUG("A2A message sent: target=%s, responses=%zu",
                  target_agent_id, response_count);
    return AIRY_SUCCESS;
}

/* ==================== 辅助接口 ==================== */

size_t a2a_service_count(a2a_service_t *svc)
{
    if (!svc || !svc->initialized)
        return 0;
    airy_mtx_lock(&svc->lock);
    size_t c = a2a_v03_get_agent_count(svc->ctx);
    airy_mtx_unlock(&svc->lock);
    return c;
}

size_t a2a_service_task_count(a2a_service_t *svc)
{
    if (!svc || !svc->initialized)
        return 0;
    airy_mtx_lock(&svc->lock);
    size_t c = a2a_v03_get_task_count(svc->ctx);
    airy_mtx_unlock(&svc->lock);
    return c;
}

void a2a_service_card_free(char *card_json)
{
    AIRY_FREE(card_json);
}

void a2a_service_task_free(char *task_json)
{
    AIRY_FREE(task_json);
}

void a2a_service_results_free(char *results_json)
{
    AIRY_FREE(results_json);
}
