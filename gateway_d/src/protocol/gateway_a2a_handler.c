// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "gateway_a2a_handler.h"

#include "airy_memory.h"

#include <stdio.h>
#include <stdlib.h>
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
        LOG_ERROR("handler allocation failed, size=%zu", sizeof(gw_a2a_handler_t));
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

int gw_a2a_handler_get_agent_card(gw_a2a_handler_t *handler, char **card_json)
{
    if (!handler || !card_json)
        return AIRY_ERR_INVALID_PARAM;

    const char *fmt = "{"
                      "\"name\":\"%s\","
                      "\"version\":\"%s\","
                      "\"url\":\"%s\","
                      "\"capabilities\":{\"taskExecution\":%s,\"streaming\":%s,"
                      "\"pushNotifications\":%s,\"negotiation\":%s,"
                      "\"multiTurn\":%s,\"stateTransition\":%s},"
                      "\"protocolVersion\":\"0.3.0\""
                      "}";

    uint32_t caps = handler->config.capabilities;
    const char *fmt_bool = "true";
    size_t len = snprintf(NULL, 0, fmt, handler->config.agent_name, handler->config.agent_version,
                          handler->config.agent_url, (caps & 0x01) ? fmt_bool : "false",
                          (caps & 0x02) ? fmt_bool : "false", (caps & 0x04) ? fmt_bool : "false",
                          (caps & 0x08) ? fmt_bool : "false", (caps & 0x10) ? fmt_bool : "false",
                          (caps & 0x20) ? fmt_bool : "false");

    char *buf = (char *)AIRY_MALLOC(len + 1);
    if (!buf)
        return AIRY_ERR_OUT_OF_MEMORY;
    snprintf(buf, len + 1, fmt, handler->config.agent_name, handler->config.agent_version,
             handler->config.agent_url, (caps & 0x01) ? fmt_bool : "false",
             (caps & 0x02) ? fmt_bool : "false", (caps & 0x04) ? fmt_bool : "false",
             (caps & 0x08) ? fmt_bool : "false", (caps & 0x10) ? fmt_bool : "false",
             (caps & 0x20) ? fmt_bool : "false");

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

static char *extract_a2a_field(const char *json, const char *field_name)
{
    if (!json || !field_name) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    size_t flen = strlen(field_name) + 4;
    char *key = (char *)AIRY_MALLOC(flen);
    if (!key) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    snprintf(key, flen, "\"%s\"", field_name);
    const char *p = strstr(json, key);
    AIRY_FREE(key);
    if (!p) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    p += strlen(field_name) + 3;
    while (*p && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (*p != '"') {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    p++;
    const char *end = strchr(p, '"');
    if (!end) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    size_t len = (size_t)(end - p);
    char *val = (char *)AIRY_MALLOC(len + 1);
    if (!val) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    AIRY_MEMCPY(val, p, len);
    val[len] = '\0';
    return val;
}

static char *extract_a2a_object_field(const char *json, const char *field_name)
{
    if (!json || !field_name)
        return AIRY_STRDUP("{}");
    size_t flen = strlen(field_name) + 4;
    char *key = (char *)AIRY_MALLOC(flen);
    if (!key)
        return AIRY_STRDUP("{}");
    snprintf(key, flen, "\"%s\"", field_name);
    const char *p = strstr(json, key);
    AIRY_FREE(key);
    if (!p)
        return AIRY_STRDUP("{}");
    p += strlen(field_name) + 3;
    while (*p && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (*p != '{' && *p != '[')
        return AIRY_STRDUP("{}");
    char open = *p;
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    const char *start = p;
    while (*p) {
        if (*p == open)
            depth++;
        else if (*p == close) {
            depth--;
            if (depth == 0) {
                p++;
                size_t len = (size_t)(p - start);
                char *obj = (char *)AIRY_MALLOC(len + 1);
                if (!obj)
                    return AIRY_STRDUP("{}");
                AIRY_MEMCPY(obj, start, len);
                obj[len] = '\0';
                return obj;
            }
        }
        p++;
    }
    return AIRY_STRDUP("{}");
}

int gw_a2a_handler_handle_request(gw_a2a_handler_t *handler, const char *method, const char *path,
                                  const char *body_json, char **response_json)
{
    if (!handler || !body_json || !response_json)
        return AIRY_ERR_INVALID_PARAM;
    handler->request_count++;

    /* 动作判定：优先 path（/a2a/agent-card、/a2a/task），
     * path 不可用时按 body 的 JSON-RPC method（tasks/send 等）判定（body-only 路由） */
    int is_agent_card = (path && strcmp(path, "/a2a/agent-card") == 0);
    int is_task = (path && strcmp(path, "/a2a/task") == 0);
    if (!is_agent_card && !is_task && body_json) {
        char *body_method = extract_a2a_field(body_json, "method");
        if (body_method) {
            if (strcmp(body_method, "tasks/send") == 0 ||
                strcmp(body_method, "task/delegate") == 0) {
                is_task = 1;
            } else if (strcmp(body_method, "agent-card/get") == 0 ||
                       strcmp(body_method, "agent/getAgentCard") == 0) {
                is_agent_card = 1;
            }
            AIRY_FREE(body_method);
        }
    }

    if (is_agent_card) {
        return gw_a2a_handler_get_agent_card(handler, response_json);
    }

    if (is_task) {
        char *task_type = extract_a2a_field(body_json, "type");
        char *task_id = extract_a2a_field(body_json, "id");
        char *input_json = extract_a2a_object_field(body_json, "message");

        if (!task_type) {
            LOG_WARN("missing task type in A2A request, path=%s", path ? path : "(null)");
            AIRY_FREE(task_id);
            AIRY_FREE(input_json);
            handler->error_count++;
            return AIRY_ERR_PARSE_ERROR;
        }

        gw_a2a_task_type_entry_t *entry = find_task_type(handler, task_type);
        if (!entry) {
            LOG_WARN("unknown task type: task_type=%s, registered=%zu", task_type,
                     handler->task_type_count);
            const char *err = "{\"error\":{\"code\":-32601,\"message\":\"Unknown task type: %s\"}}";
            size_t elen = snprintf(NULL, 0, err, task_type);
            char *ebuf = (char *)AIRY_MALLOC(elen + 1);
            if (ebuf)
                snprintf(ebuf, elen + 1, err, task_type);
            *response_json = ebuf;
            AIRY_FREE(task_type);
            AIRY_FREE(task_id);
            AIRY_FREE(input_json);
            handler->error_count++;
            return AIRY_ERR_NOT_FOUND;
        }

        char *output = NULL;
        int rc = entry->exec_fn(task_id ? task_id : "unknown", task_type,
                                input_json ? input_json : "{}", &output, entry->user_data);

        if (rc != 0 || !output) {
            LOG_ERROR("task execution failed: task_type=%s, rc=%d", task_type, rc);
            AIRY_FREE(task_type);
            AIRY_FREE(task_id);
            AIRY_FREE(input_json);
            AIRY_FREE(output);
            handler->error_count++;
            return AIRY_ERR_EXEC_FAIL;
        }

        AIRY_FREE(task_type);
        task_type = NULL;
        AIRY_FREE(task_id);
        task_id = NULL;
        AIRY_FREE(input_json);
        input_json = NULL;

        const char *resp_fmt = "{\"result\":{\"status\":\"completed\",\"output\":%s}}";
        size_t rlen = snprintf(NULL, 0, resp_fmt, output);
        char *buf = (char *)AIRY_MALLOC(rlen + 1);
        if (buf)
            snprintf(buf, rlen + 1, resp_fmt, output);
        *response_json = buf;
        AIRY_FREE(output);
        return 0;
    }

    if (strstr(body_json, "\"agentCard\"") || strstr(body_json, "\"agent-card\"")) {
        return gw_a2a_handler_get_agent_card(handler, response_json);
    }

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

bool gw_a2a_handler_is_healthy(gw_a2a_handler_t *handler)
{
    if (!handler)
        return false;
    return handler->healthy;
}
