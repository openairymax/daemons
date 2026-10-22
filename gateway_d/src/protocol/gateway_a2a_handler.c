// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "gateway_a2a_handler.h"

#include "gateway_jsonpick.h"
#include "airy_memory.h"

#include <string.h>
#include "error.h"

#include "logging.h"

#define GW_A2A_MAX_TASK_TYPES 64

typedef struct {
    char task_type[128];
    gw_a2a_task_exec_fn exec_fn;
    void *user_data;
} gw_a2a_task_type_entry_t;

struct gw_a2a_handler {
    gw_a2a_handler_config_t config;
    gw_a2a_task_type_entry_t task_types[GW_A2A_MAX_TASK_TYPES];
    size_t task_type_count;
    bool initialized;
    bool healthy;
    uint64_t request_count;
    uint64_t error_count;
};

static int handle_a2a_request(const char *method, const char *path, const char *body_json,
                              char **response_json, void *user_data);

gw_a2a_handler_t *gw_a2a_handler_create(const gw_a2a_handler_config_t *config)
{
    gw_a2a_handler_t *handler = (gw_a2a_handler_t *)AIRY_CALLOC(1, sizeof(gw_a2a_handler_t));
    if (!handler) {
        AIRY_LOG_ERROR("handler allocation failed, size=%zu", sizeof(gw_a2a_handler_t));
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    if (config) {
        handler->config = *config;
    } else {
        gw_a2a_handler_config_t defaults = GW_A2A_HANDLER_CONFIG_DEFAULTS;
        handler->config = defaults;
    }
    return handler;
}

void gw_a2a_handler_destroy(gw_a2a_handler_t *handler)
{
    if (!handler)
        return;
    if (handler->initialized) {
        gw_a2a_handler_shutdown(handler);
    }
    AIRY_FREE(handler);
}

int gw_a2a_handler_init(gw_a2a_handler_t *handler)
{
    if (!handler)
        return AIRY_ERR_INVALID_PARAM;
    if (handler->initialized)
        return 0;
    handler->initialized = true;
    handler->healthy = true;
    handler->request_count = 0;
    handler->error_count = 0;
    return 0;
}

int gw_a2a_handler_shutdown(gw_a2a_handler_t *handler)
{
    if (!handler || !handler->initialized)
        return AIRY_ERR_INVALID_PARAM;
    handler->task_type_count = 0;
    handler->initialized = false;
    handler->healthy = false;
    return 0;
}

int gw_a2a_handler_register_task_type(gw_a2a_handler_t *handler, const char *task_type,
                                      gw_a2a_task_exec_fn exec_fn, void *user_data)
{
    if (!handler || !task_type || !exec_fn)
        return AIRY_ERR_INVALID_PARAM;
    if (handler->task_type_count >= GW_A2A_MAX_TASK_TYPES)
        return AIRY_ERR_OVERFLOW;

    gw_a2a_task_type_entry_t *entry = &handler->task_types[handler->task_type_count];
    AIRY_STRNCPY_TERM(entry->task_type, task_type, sizeof(entry->task_type));
    entry->task_type[sizeof(entry->task_type) - 1] = '\0';
    entry->exec_fn = exec_fn;
    entry->user_data = user_data;
    handler->task_type_count++;
    return 0;
}

/* 六位能力旗标展开为 JSON 布尔字面量，供卡体格式化单点取值。 */
static void a2a_caps_words(uint32_t caps, const char *out[6])
{
    for (size_t i = 0; i < 6; i++)
        out[i] = (caps & (1u << i)) ? "true" : "false";
}

static int gw_a2a_handler_get_agent_card(gw_a2a_handler_t *handler, char **card_json)
{
    if (!handler || !card_json)
        return AIRY_ERR_INVALID_PARAM;

    const char *cap[6];
    a2a_caps_words(handler->config.capabilities, cap);
    char *buf = gw_fmt_alloc("{\"name\":\"%s\",\"version\":\"%s\",\"url\":\"%s\","
                             "\"capabilities\":{\"taskExecution\":%s,\"streaming\":%s,"
                             "\"pushNotifications\":%s,\"negotiation\":%s,"
                             "\"multiTurn\":%s,\"stateTransition\":%s},"
                             "\"protocolVersion\":\"0.3.0\""
                             "}",
                             handler->config.agent_name, handler->config.agent_version,
                             handler->config.agent_url, cap[0], cap[1], cap[2], cap[3], cap[4],
                             cap[5]);
    if (!buf)
        return AIRY_ERR_OUT_OF_MEMORY;
    *card_json = buf;
    return 0;
}

static gw_a2a_task_type_entry_t *find_task_type(gw_a2a_handler_t *handler, const char *task_type)
{
    for (size_t i = 0; i < handler->task_type_count; i++) {
        if (strcmp(handler->task_types[i].task_type, task_type) == 0) {
            return &handler->task_types[i];
        }
    }
    AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
}

/* 路由判定：路径优先（/a2a/agent-card、/a2a/task）；无路径时依 JSON-RPC
 * method 或卡体标记兜底。 */
static void a2a_route(const char *path, const char *body_json, int *is_card, int *is_task)
{
    *is_card = (path && strcmp(path, "/a2a/agent-card") == 0);
    *is_task = (path && strcmp(path, "/a2a/task") == 0);
    if (*is_card || *is_task)
        return;

    cJSON *root = gw_json_load(body_json);
    char *method = gw_json_str(root, "method");
    cJSON_Delete(root);
    if (method) {
        if (strcmp(method, "tasks/send") == 0 || strcmp(method, "task/delegate") == 0)
            *is_task = 1;
        else if (strcmp(method, "agent-card/get") == 0 ||
                 strcmp(method, "agent/getAgentCard") == 0)
            *is_card = 1;
        AIRY_FREE(method);
    }
    if (!*is_card && !*is_task &&
        (strstr(body_json, "\"agentCard\"") || strstr(body_json, "\"agent-card\"")))
        *is_card = 1;
}

/* 查表执行已注册任务类型并封装完成体；未知类型回填错误体。 */
static int a2a_exec_task(gw_a2a_handler_t *handler, const char *task_type, const char *task_id,
                         const char *input_json, char **response_json)
{
    gw_a2a_task_type_entry_t *entry = find_task_type(handler, task_type);
    if (!entry) {
        AIRY_LOG_WARN("unknown task type: task_type=%s, registered=%zu", task_type,
                      handler->task_type_count);
        *response_json = gw_fmt_alloc(
            "{\"error\":{\"code\":-32601,\"message\":\"Unknown task type: %s\"}}", task_type);
        handler->error_count++;
        return AIRY_ERR_NOT_FOUND;
    }

    char *output = NULL;
    int rc = entry->exec_fn(task_id ? task_id : "unknown", task_type, input_json, &output,
                            entry->user_data);
    if (rc != 0 || !output) {
        AIRY_LOG_ERROR("task execution failed: task_type=%s, rc=%d", task_type, rc);
        AIRY_FREE(output);
        handler->error_count++;
        return AIRY_ERR_EXEC_FAIL;
    }
    *response_json = gw_fmt_alloc("{\"result\":{\"status\":\"completed\",\"output\":%s}}", output);
    AIRY_FREE(output);
    return 0;
}

/* 任务分支：解析请求体三字段 → 执行 → 释放临时副本。 */
static int a2a_run_task(gw_a2a_handler_t *handler, const char *path, const char *body_json,
                        char **response_json)
{
    cJSON *root = gw_json_load(body_json);
    char *task_type = gw_json_str(root, "type");
    char *task_id = gw_json_str(root, "id");
    char *input_json = gw_json_raw(root, "message");
    cJSON_Delete(root);
    if (!input_json)
        input_json = AIRY_STRDUP("{}");

    int rc;
    if (!task_type) {
        AIRY_LOG_WARN("missing task type in A2A request, path=%s", path ? path : "(null)");
        handler->error_count++;
        rc = AIRY_ERR_PARSE_ERROR;
    } else {
        rc = a2a_exec_task(handler, task_type, task_id, input_json, response_json);
    }
    AIRY_FREE(task_type);
    AIRY_FREE(task_id);
    AIRY_FREE(input_json);
    return rc;
}

static int gw_a2a_handler_handle_request(gw_a2a_handler_t *handler, const char *method,
                                         const char *path, const char *body_json,
                                         char **response_json)
{
    (void)method;
    if (!handler || !body_json || !response_json)
        return AIRY_ERR_INVALID_PARAM;
    handler->request_count++;

    int is_card = 0;
    int is_task = 0;
    a2a_route(path, body_json, &is_card, &is_task);
    if (is_card)
        return gw_a2a_handler_get_agent_card(handler, response_json);
    if (is_task)
        return a2a_run_task(handler, path, body_json, response_json);
    handler->error_count++;
    return AIRY_ERR_NOT_FOUND;
}

static int handle_a2a_request(const char *method, const char *path, const char *body_json,
                              char **response_json, void *user_data)
{
    gw_a2a_handler_t *handler = (gw_a2a_handler_t *)user_data;
    if (!handler)
        return AIRY_ERR_NULL_POINTER;
    return gw_a2a_handler_handle_request(handler, method, path, body_json, response_json);
}

gw_proto_request_handler_t gw_a2a_handler_get_handler(gw_a2a_handler_t *handler)
{
    if (!handler) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }
    return handle_a2a_request;
}

void *gw_a2a_handler_get_handler_data(gw_a2a_handler_t *handler)
{
    return (void *)handler;
}
