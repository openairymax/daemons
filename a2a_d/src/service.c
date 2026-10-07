/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * @file service.c
 * @brief A2A service implementation consuming the injected protocol adapter.
 *
 * 服务核只经 proto_registry 解析并持有 protocol_adapter_t 句柄，所有业务
 * 操作经统一信封（method + payload）转发给适配器，本层不含任何厂商载荷，
 * 实现机制（daemon）与策略（适配器）的完全解耦。
 *
 * Design notes:
 * - Adapter resolved once in create() via proto_registry_get()/find()
 * - Every operation serializes params as cJSON and dispatches handle_request
 * - Thread safety: a2a_call() holds the lock around each adapter call
 * - Result JSON ownership transfers to the caller (a2a_service_result_free)
 */

#include "service.h"

#include "error.h"
#include "protocol_registry.h"
#include "svc_logger.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>

static void a2a_set_str(cJSON *obj, const char *key, const char *val)
{
    if (val)
        cJSON_AddStringToObject(obj, key, val);
    else
        cJSON_AddNullToObject(obj, key);
}

static char *a2a_pack(cJSON *obj)
{
    if (!obj)
        return NULL;
    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return str;
}

static int a2a_call(a2a_service_t *svc, const char *method, const char *params_json,
                    char **out_json)
{
    if (!svc || !svc->initialized || !svc->adapter || !method || !out_json)
        return AIRY_ERR_INVALID_PARAM;

    *out_json = NULL;

    unified_message_t msg;
    __builtin_memset(&msg, 0, sizeof(msg));
    snprintf(msg.method, sizeof(msg.method), "%s", method);
    const char *params = (params_json && params_json[0]) ? params_json : "{}";
    msg.payload = (void *)params;
    msg.payload_size = strlen(params);

    void *resp = NULL;
    airy_mtx_lock(&svc->lock);
    int rc = svc->adapter->handle_request(svc->context, &msg, &resp);
    airy_mtx_unlock(&svc->lock);

    if (rc != AIRY_SUCCESS) {
        AIRY_FREE(resp);
        return rc;
    }
    if (!resp)
        return AIRY_ERR_OUT_OF_MEMORY;

    *out_json = (char *)resp;
    return AIRY_SUCCESS;
}

static int a2a_op(a2a_service_t *svc, const char *method, cJSON *params, char **out_json)
{
    char *params_json = a2a_pack(params);
    if (!params_json)
        return AIRY_ERR_OUT_OF_MEMORY;
    int rc = a2a_call(svc, method, params_json, out_json);
    AIRY_FREE(params_json);
    return rc;
}

a2a_service_t *a2a_service_create(void)
{
    int rc = proto_interface_register_builtins();
    if (rc < 0) {
        SVC_LOG_ERROR("A2A protocol builtins registration failed: rc=%d", rc);
        return NULL;
    }

    protocol_registry_t *registry = proto_registry_get();
    proto_registry_entry_t *entry = registry ? proto_registry_find(registry, "A2A") : NULL;
    if (!entry || !entry->adapter || !entry->adapter->handle_request) {
        SVC_LOG_ERROR("A2A adapter unavailable in protocol registry");
        return NULL;
    }

    a2a_service_t *svc = (a2a_service_t *)AIRY_CALLOC(1, sizeof(a2a_service_t));
    if (!svc)
        return NULL;

    svc->adapter = entry->adapter;
    svc->context = entry->context;
    airy_mtx_init(&svc->lock);

    if (svc->adapter->init && svc->adapter->init(svc->context) != AIRY_SUCCESS) {
        airy_mtx_destroy(&svc->lock);
        AIRY_FREE(svc);
        SVC_LOG_ERROR("A2A adapter init failed");
        return NULL;
    }

    svc->initialized = 1;
    SVC_LOG_INFO("A2A service created via protocol registry");
    return svc;
}

void a2a_service_destroy(a2a_service_t *svc)
{
    if (!svc)
        return;

    airy_mtx_lock(&svc->lock);
    if (svc->adapter && svc->adapter->destroy)
        svc->adapter->destroy(svc->context);
    svc->adapter = NULL;
    svc->context = NULL;
    svc->initialized = 0;
    airy_mtx_unlock(&svc->lock);

    airy_mtx_destroy(&svc->lock);
    AIRY_FREE(svc);
}

int a2a_service_register_agent(a2a_service_t *svc, const char *card_json, char **out_result_json)
{
    if (!svc || !svc->initialized || !card_json || !out_result_json)
        return AIRY_ERR_INVALID_PARAM;
    return a2a_call(svc, "register_agent", card_json, out_result_json);
}

int a2a_service_unregister_agent(a2a_service_t *svc, const char *agent_id, char **out_result_json)
{
    if (!svc || !svc->initialized || !agent_id || !out_result_json)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return AIRY_ERR_OUT_OF_MEMORY;
    a2a_set_str(params, "agent_id", agent_id);
    return a2a_op(svc, "unregister_agent", params, out_result_json);
}

int a2a_service_get_agent_card(a2a_service_t *svc, const char *agent_id, char **out_result_json)
{
    if (!svc || !svc->initialized || !agent_id || !out_result_json)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return AIRY_ERR_OUT_OF_MEMORY;
    a2a_set_str(params, "agent_id", agent_id);
    return a2a_op(svc, "get_agent_card", params, out_result_json);
}

int a2a_service_discover_agents(a2a_service_t *svc, const char *capability, const char *skill_name,
                                char **out_result_json)
{
    if (!svc || !svc->initialized || !out_result_json)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return AIRY_ERR_OUT_OF_MEMORY;
    a2a_set_str(params, "capability", capability);
    a2a_set_str(params, "skill", skill_name);
    return a2a_op(svc, "discover_agents", params, out_result_json);
}

int a2a_service_create_task(a2a_service_t *svc, const char *agent_id, const char *description,
                            const char *input_json, char **out_result_json)
{
    if (!svc || !svc->initialized || !agent_id || !out_result_json)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return AIRY_ERR_OUT_OF_MEMORY;
    a2a_set_str(params, "agent_id", agent_id);
    a2a_set_str(params, "description", description);
    a2a_set_str(params, "input", input_json);
    return a2a_op(svc, "create_task", params, out_result_json);
}

int a2a_service_update_task(a2a_service_t *svc, const char *task_id, int state,
                            const char *output_json, double progress, char **out_result_json)
{
    if (!svc || !svc->initialized || !task_id || !out_result_json)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return AIRY_ERR_OUT_OF_MEMORY;
    a2a_set_str(params, "task_id", task_id);
    cJSON_AddNumberToObject(params, "state", (double)state);
    a2a_set_str(params, "output", output_json);
    cJSON_AddNumberToObject(params, "progress", progress);
    return a2a_op(svc, "update_task", params, out_result_json);
}

int a2a_service_cancel_task(a2a_service_t *svc, const char *task_id, const char *reason,
                            char **out_result_json)
{
    if (!svc || !svc->initialized || !task_id || !out_result_json)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return AIRY_ERR_OUT_OF_MEMORY;
    a2a_set_str(params, "task_id", task_id);
    a2a_set_str(params, "reason", reason);
    return a2a_op(svc, "cancel_task", params, out_result_json);
}

int a2a_service_get_task(a2a_service_t *svc, const char *task_id, char **out_result_json)
{
    if (!svc || !svc->initialized || !task_id || !out_result_json)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return AIRY_ERR_OUT_OF_MEMORY;
    a2a_set_str(params, "task_id", task_id);
    return a2a_op(svc, "get_task", params, out_result_json);
}

int a2a_service_send_message(a2a_service_t *svc, const char *target_agent_id, const char *role,
                             const char *content_json, char **out_result_json)
{
    if (!svc || !svc->initialized || !target_agent_id || !role || !content_json ||
        !out_result_json)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return AIRY_ERR_OUT_OF_MEMORY;
    a2a_set_str(params, "target_agent_id", target_agent_id);
    a2a_set_str(params, "role", role);
    a2a_set_str(params, "content", content_json);
    return a2a_op(svc, "send_message", params, out_result_json);
}

int a2a_service_stats(a2a_service_t *svc, size_t *out_agents, size_t *out_tasks)
{
    if (!svc || !svc->initialized)
        return AIRY_ERR_INVALID_PARAM;

    char *result = NULL;
    int rc = a2a_call(svc, "count", NULL, &result);
    if (rc != AIRY_SUCCESS)
        return rc;
    if (!result)
        return AIRY_ERR_OUT_OF_MEMORY;

    cJSON *root = cJSON_Parse(result);
    AIRY_FREE(result);
    if (!root)
        return AIRY_ERR_INVALID_PARAM;

    if (out_agents) {
        cJSON *item = cJSON_GetObjectItem(root, "agent_count");
        *out_agents = (item && cJSON_IsNumber(item)) ? (size_t)item->valuedouble : 0;
    }
    if (out_tasks) {
        cJSON *item = cJSON_GetObjectItem(root, "task_count");
        *out_tasks = (item && cJSON_IsNumber(item)) ? (size_t)item->valuedouble : 0;
    }

    cJSON_Delete(root);
    return AIRY_SUCCESS;
}

void a2a_service_result_free(char *result_json)
{
    AIRY_FREE(result_json);
}
