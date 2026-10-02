// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "gateway_mcp_server.h"

#include "gateway_jsonpick.h"
#include "airy_memory.h"

#include <stdio.h>
#include <string.h>
#include "error.h"

#include "logging.h"

#define GW_MCP_MAX_TOOLS 256
#define GW_MCP_MAX_RESOURCES 128
#define GW_MCP_MAX_PROMPTS 64

typedef struct {
    char name[128];
    char description[512];
    char input_schema[2048];
    gw_mcp_tool_exec_fn exec_fn;
    void *user_data;
} gw_mcp_tool_entry_t;

typedef struct {
    char uri[512];
    char name[256];
    char description[512];
    char mime_type[64];
    gw_mcp_resource_read_fn read_fn;
    void *user_data;
} gw_mcp_resource_entry_t;

struct gw_mcp_server {
    gw_mcp_server_config_t config;
    gw_mcp_tool_entry_t tools[GW_MCP_MAX_TOOLS];
    size_t tool_count;
    gw_mcp_resource_entry_t resources[GW_MCP_MAX_RESOURCES];
    size_t resource_count;
    bool initialized;
    bool healthy;
    uint64_t request_count;
    uint64_t error_count;
};

static int handle_mcp_request(const char *method, const char *path, const char *body_json,
                              char **response_json, void *user_data);

gw_mcp_server_t *gw_mcp_server_create(const gw_mcp_server_config_t *config)
{
    gw_mcp_server_t *server = (gw_mcp_server_t *)AIRY_CALLOC(1, sizeof(gw_mcp_server_t));
    if (!server) {
        AIRY_LOG_ERROR("server allocation failed, size=%zu", sizeof(gw_mcp_server_t));
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    if (config) {
        server->config = *config;
    } else {
        gw_mcp_server_config_t defaults = GW_MCP_SERVER_CONFIG_DEFAULTS;
        server->config = defaults;
    }
    return server;
}

void gw_mcp_server_destroy(gw_mcp_server_t *server)
{
    if (!server)
        return;
    if (server->initialized) {
        gw_mcp_server_shutdown(server);
    }
    AIRY_FREE(server);
}

int gw_mcp_server_init(gw_mcp_server_t *server)
{
    if (!server)
        return AIRY_ERR_INVALID_PARAM;
    if (server->initialized)
        return 0;
    server->initialized = true;
    server->healthy = true;
    server->request_count = 0;
    server->error_count = 0;
    return 0;
}

int gw_mcp_server_shutdown(gw_mcp_server_t *server)
{
    if (!server || !server->initialized)
        return AIRY_ERR_INVALID_PARAM;
    server->tool_count = 0;
    server->resource_count = 0;
    server->initialized = false;
    server->healthy = false;
    return 0;
}

int gw_mcp_server_register_tool(gw_mcp_server_t *server, const char *name, const char *description,
                                const char *input_schema_json, gw_mcp_tool_exec_fn exec_fn,
                                void *user_data)
{
    if (!server || !name || !exec_fn)
        return AIRY_ERR_INVALID_PARAM;
    if (server->tool_count >= GW_MCP_MAX_TOOLS)
        return AIRY_ERR_OVERFLOW;

    gw_mcp_tool_entry_t *entry = &server->tools[server->tool_count];
    AIRY_STRNCPY_TERM(entry->name, name, sizeof(entry->name));
    entry->name[sizeof(entry->name) - 1] = '\0';
    if (description) {
        AIRY_STRNCPY_TERM(entry->description, description, sizeof(entry->description));
        entry->description[sizeof(entry->description) - 1] = '\0';
    }
    if (input_schema_json) {
        AIRY_STRNCPY_TERM(entry->input_schema, input_schema_json, sizeof(entry->input_schema));
        entry->input_schema[sizeof(entry->input_schema) - 1] = '\0';
    }
    entry->exec_fn = exec_fn;
    entry->user_data = user_data;
    server->tool_count++;
    return 0;
}

int gw_mcp_server_register_resource(gw_mcp_server_t *server, const char *uri, const char *name,
                                    const char *description, const char *mime_type,
                                    gw_mcp_resource_read_fn read_fn, void *user_data)
{
    if (!server || !uri || !read_fn)
        return AIRY_ERR_INVALID_PARAM;
    if (server->resource_count >= GW_MCP_MAX_RESOURCES)
        return AIRY_ERR_OVERFLOW;

    gw_mcp_resource_entry_t *entry = &server->resources[server->resource_count];
    AIRY_STRNCPY_TERM(entry->uri, uri, sizeof(entry->uri));
    entry->uri[sizeof(entry->uri) - 1] = '\0';
    if (name) {
        AIRY_STRNCPY_TERM(entry->name, name, sizeof(entry->name));
        entry->name[sizeof(entry->name) - 1] = '\0';
    }
    if (description) {
        AIRY_STRNCPY_TERM(entry->description, description, sizeof(entry->description));
        entry->description[sizeof(entry->description) - 1] = '\0';
    }
    if (mime_type) {
        AIRY_STRNCPY_TERM(entry->mime_type, mime_type, sizeof(entry->mime_type));
        entry->mime_type[sizeof(entry->mime_type) - 1] = '\0';
    }
    entry->read_fn = read_fn;
    entry->user_data = user_data;
    server->resource_count++;
    return 0;
}

static gw_mcp_tool_entry_t *find_tool(gw_mcp_server_t *server, const char *name)
{
    for (size_t i = 0; i < server->tool_count; i++) {
        if (strcmp(server->tools[i].name, name) == 0) {
            return &server->tools[i];
        }
    }
    AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
}

static gw_mcp_resource_entry_t *find_resource(gw_mcp_server_t *server, const char *uri)
{
    for (size_t i = 0; i < server->resource_count; i++) {
        if (strcmp(server->resources[i].uri, uri) == 0) {
            return &server->resources[i];
        }
    }
    AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
}

static char *build_tools_list_json(gw_mcp_server_t *server)
{

    size_t buf_size = 4096 + server->tool_count * 4096;
    char *buf = (char *)AIRY_MALLOC(buf_size);
    if (!buf) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null buffer");
    }

    size_t pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, "{\"tools\":[");
    for (size_t i = 0; i < server->tool_count; i++) {
        if (i > 0)
            pos += snprintf(buf + pos, buf_size - pos, ",");
        gw_mcp_tool_entry_t *t = &server->tools[i];
        /* MCP tools/list spec: every tool must carry an inputSchema (the JSON
         * object registered at registration time, embedded as-is; invalid or
         * overlong JSON is truncated), fixing the problem of clients being
         * unable to obtain parameter definitions and thus unable to call tools
         * correctly */
        pos += snprintf(buf + pos, buf_size - pos,
                        "{\"name\":\"%s\",\"description\":\"%s\",\"inputSchema\":%.*s}", t->name,
                        t->description, (int)sizeof(t->input_schema) - 1, t->input_schema);
    }
    pos += snprintf(buf + pos, buf_size - pos, "]}");
    return buf;
}

static char *build_resources_list_json(gw_mcp_server_t *server)
{
    size_t buf_size = 4096 + server->resource_count * 1024;
    char *buf = (char *)AIRY_MALLOC(buf_size);
    if (!buf) {
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null buffer");
    }

    size_t pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, "{\"resources\":[");
    for (size_t i = 0; i < server->resource_count; i++) {
        if (i > 0)
            pos += snprintf(buf + pos, buf_size - pos, ",");
        gw_mcp_resource_entry_t *r = &server->resources[i];
        pos +=
            snprintf(buf + pos, buf_size - pos,
                     "{\"uri\":\"%s\",\"name\":\"%s\",\"description\":\"%s\",\"mimeType\":\"%s\"}",
                     r->uri, r->name, r->description, r->mime_type);
    }
    pos += snprintf(buf + pos, buf_size - pos, "]}");
    return buf;
}

/**
 * @brief JSON-RPC processing core (with request-id echo)
 *
 * @param id_json Raw JSON text of the request id (numbers as-is / strings with
 *                quotes), safely embeddable in the response ("id":%s); NULL
 *                falls back to "id":null. MCP is based on JSON-RPC 2.0: the
 *                response id must match the request id, otherwise clients
 *                cannot correlate the response to the original request.
 */
static int gw_mcp_server_handle_jsonrpc_ex(gw_mcp_server_t *server, const char *method,
                                           const char *params_json, const char *id_json,
                                           char **response_json)
{
    if (!server || !method || !response_json)
        return AIRY_ERR_INVALID_PARAM;
    server->request_count++;
    const char *rid = id_json ? id_json : "null";

    if (strcmp(method, "initialize") == 0) {
        const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                           "\"protocolVersion\":\"2024-11-05\","
                           "\"capabilities\":{\"tools\":{\"listChanged\":true},"
                           "\"resources\":{\"subscribe\":true,\"listChanged\":true}},"
                           "\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"}}}";
        size_t len =
            snprintf(NULL, 0, resp, rid, server->config.server_name, server->config.server_version);
        char *buf = (char *)AIRY_MALLOC(len + 1);
        if (!buf) {
            server->error_count++;
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        snprintf(buf, len + 1, resp, rid, server->config.server_name,
                 server->config.server_version);
        *response_json = buf;
        return 0;
    }

    if (strcmp(method, "tools/list") == 0) {
        /* MCP spec: the tools/list response must be a JSON-RPC result
         * wrapper {"jsonrpc","id","result":{"tools":[...]}} so external
         * standard clients can parse it. */
        char *inner = build_tools_list_json(server);
        if (!inner) {
            server->error_count++;
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        const char *resp_fmt = "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}";
        size_t rlen = snprintf(NULL, 0, resp_fmt, rid, inner);
        char *buf = (char *)AIRY_MALLOC(rlen + 1);
        if (buf) {
            snprintf(buf, rlen + 1, resp_fmt, rid, inner);
        }
        AIRY_FREE(inner);
        *response_json = buf;
        return 0;
    }

    if (strcmp(method, "tools/call") == 0) {
        cJSON *proot = gw_json_load(params_json);
        char *tool_name = gw_json_str(proot, "name");
        char *tool_args = gw_json_raw(proot, "arguments");
        cJSON_Delete(proot);
        if (!tool_args)
            tool_args = AIRY_STRDUP("{}");
        if (!tool_name) {
            AIRY_LOG_WARN("failed to extract tool name from params in tools/call");
            AIRY_FREE(tool_args);
            server->error_count++;
            return AIRY_ERR_PARSE_ERROR;
        }
        gw_mcp_tool_entry_t *tool = find_tool(server, tool_name);
        if (!tool) {
            AIRY_LOG_WARN("tool not found: tool_name=%s, tool_count=%zu", tool_name, server->tool_count);
            const char *err = "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":"
                              "{\"code\":-32601,\"message\":\"Tool not found: %s\"}}";
            const char *id_json = rid ? rid : "null";
            size_t elen = snprintf(NULL, 0, err, id_json, tool_name);
            char *ebuf = (char *)AIRY_MALLOC(elen + 1);
            if (ebuf)
                snprintf(ebuf, elen + 1, err, id_json, tool_name);
            *response_json = ebuf;
            AIRY_FREE(tool_name);
            AIRY_FREE(tool_args);
            server->error_count++;
            return AIRY_ERR_NOT_FOUND;
        }
        char *tool_result = NULL;
        int rc = tool->exec_fn(tool_name, tool_args, &tool_result, tool->user_data);
        if (rc != 0 || !tool_result) {
            AIRY_LOG_ERROR("tool execution failed: tool_name=%s, rc=%d", tool_name, rc);
            AIRY_FREE(tool_name);
            tool_name = NULL;
            AIRY_FREE(tool_args);
            tool_args = NULL;
            const char *err = "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":"
                              "{\"code\":-32603,\"message\":\"Tool execution failed\"}}";
            const char *id_json = rid ? rid : "null";
            size_t elen = snprintf(NULL, 0, err, id_json);
            char *ebuf = (char *)AIRY_MALLOC(elen + 1);
            if (ebuf)
                snprintf(ebuf, elen + 1, err, id_json);
            *response_json = ebuf;
            AIRY_FREE(tool_result);
            server->error_count++;
            return AIRY_ERR_EXEC_FAIL;
        }
        AIRY_FREE(tool_name);
        tool_name = NULL;
        AIRY_FREE(tool_args);
        tool_args = NULL;
        const char *resp_fmt = "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                               "\"content\":[{\"type\":\"text\",\"text\":%s}]}}";
        size_t rlen = snprintf(NULL, 0, resp_fmt, rid, tool_result);
        char *buf = (char *)AIRY_MALLOC(rlen + 1);
        if (buf)
            snprintf(buf, rlen + 1, resp_fmt, rid, tool_result);
        *response_json = buf;
        AIRY_FREE(tool_result);
        return 0;
    }

    if (strcmp(method, "resources/list") == 0) {

        char *inner = build_resources_list_json(server);
        if (!inner) {
            server->error_count++;
            return AIRY_ERR_OUT_OF_MEMORY;
        }
        const char *resp_fmt = "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}";
        size_t rlen = snprintf(NULL, 0, resp_fmt, rid, inner);
        char *buf = (char *)AIRY_MALLOC(rlen + 1);
        if (buf) {
            snprintf(buf, rlen + 1, resp_fmt, rid, inner);
        }
        AIRY_FREE(inner);
        *response_json = buf;
        return 0;
    }

    if (strcmp(method, "resources/read") == 0) {
        cJSON *proot = gw_json_load(params_json);
        char *uri = gw_json_str(proot, "uri");
        cJSON_Delete(proot);
        if (!uri) {
            AIRY_LOG_WARN("failed to extract URI from params in resources/read");
            server->error_count++;
            return AIRY_ERR_PARSE_ERROR;
        }
        gw_mcp_resource_entry_t *res = find_resource(server, uri);
        if (!res) {
            AIRY_LOG_WARN("resource not found: uri=%s, resource_count=%zu", uri, server->resource_count);
            AIRY_FREE(uri);
            server->error_count++;
            return AIRY_ERR_NOT_FOUND;
        }
        char *content = NULL;
        char *mime = NULL;
        int rc = res->read_fn(uri, &content, &mime, res->user_data);
        AIRY_FREE(uri);
        uri = NULL;
        if (rc != 0 || !content) {
            AIRY_LOG_ERROR("resource read failed: uri=%s, rc=%d", res->uri, rc);
            AIRY_FREE(content);
            AIRY_FREE(mime);
            server->error_count++;
            return AIRY_ERR_IO;
        }
        const char *resp_fmt = "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                               "\"contents\":[{\"uri\":\"%s\",\"mimeType\":\"%s\",\"text\":%s}]}}";
        const char *id_json = rid ? rid : "null";
        size_t rlen = snprintf(NULL, 0, resp_fmt, id_json, res->uri, mime ? mime : "text/plain", content);
        char *buf = (char *)AIRY_MALLOC(rlen + 1);
        if (buf)
            snprintf(buf, rlen + 1, resp_fmt, id_json, res->uri, mime ? mime : "text/plain", content);
        *response_json = buf;
        AIRY_FREE(content);
        AIRY_FREE(mime);
        return 0;
    }

    if (strcmp(method, "ping") == 0) {
        const char *id_json = rid ? rid : "null";
        size_t rlen = snprintf(NULL, 0, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{}}", id_json);
        char *buf = (char *)AIRY_MALLOC(rlen + 1);
        if (buf)
            snprintf(buf, rlen + 1, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{}}", id_json);
        *response_json = buf;
        return 0;
    }

    server->error_count++;
    return AIRY_ERR_NOT_FOUND;
}

int gw_mcp_server_handle_jsonrpc(gw_mcp_server_t *server, const char *method,
                                 const char *params_json, char **response_json)
{
    return gw_mcp_server_handle_jsonrpc_ex(server, method, params_json, NULL, response_json);
}

int gw_mcp_server_handle_request(gw_mcp_server_t *server, const char *method, const char *path,
                                 const char *body_json, char **response_json)
{
    if (!server || !body_json || !response_json)
        return AIRY_ERR_INVALID_PARAM;

    cJSON *root = gw_json_load(body_json);
    if (!root) {
        AIRY_LOG_WARN("failed to extract JSON-RPC method from request body");
        server->error_count++;
        return AIRY_ERR_PARSE_ERROR;
    }

    char *rpc_method = gw_json_str(root, "method");
    char *rpc_params = gw_json_raw(root, "params");
    if (!rpc_params)
        rpc_params = AIRY_STRDUP("{}");
    char *rpc_id = gw_json_raw(root, "id");
    cJSON_Delete(root);

    if (!rpc_method) {
        AIRY_LOG_WARN("failed to extract JSON-RPC method from request body");
        AIRY_FREE(rpc_params);
        AIRY_FREE(rpc_id);
        server->error_count++;
        return AIRY_ERR_PARSE_ERROR;
    }

    int rc = gw_mcp_server_handle_jsonrpc_ex(server, rpc_method, rpc_params, rpc_id, response_json);
    AIRY_FREE(rpc_id);
    AIRY_FREE(rpc_method);
    AIRY_FREE(rpc_params);
    return rc;
}

static int handle_mcp_request(const char *method, const char *path, const char *body_json,
                              char **response_json, void *user_data)
{
    gw_mcp_server_t *server = (gw_mcp_server_t *)user_data;
    if (!server)
        return AIRY_ERR_NULL_POINTER;
    return gw_mcp_server_handle_request(server, method, path, body_json, response_json);
}

gw_proto_request_handler_t gw_mcp_server_get_handler(gw_mcp_server_t *server)
{
    if (!server) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    return handle_mcp_request;
}

void *gw_mcp_server_get_handler_data(gw_mcp_server_t *server)
{
    return (void *)server;
}

bool gw_mcp_server_is_healthy(gw_mcp_server_t *server)
{
    if (!server)
        return false;
    return server->healthy;
}
