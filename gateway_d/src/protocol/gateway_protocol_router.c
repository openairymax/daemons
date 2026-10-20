// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

#include "gateway_protocol_router.h"

#include "gateway_a2a_handler.h"
#include "gateway_mcp_server.h"
#include "gateway_llm_api.h"
#include "airy_memory.h"

#include "protocol_detect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "error.h"

#include "logging.h"

static const char *proto_type_name(gw_proto_detect_result_t proto_type)
{
    switch (proto_type) {
    case GW_PROTO_DETECT_MCP:
        return "MCP";
    case GW_PROTO_DETECT_A2A:
        return "A2A";
    case GW_PROTO_DETECT_LLM_API:
        return "LLM-API";
    case GW_PROTO_DETECT_JSONRPC:
        return "JSON-RPC";
    default:
        return "Unknown";
    }
}

typedef struct {
    gw_proto_detect_result_t proto_type;
    gw_proto_request_handler_t handler;
    void *user_data;
    bool registered;
} gw_proto_adapter_entry_t;

struct gw_proto_router {
    gw_proto_adapter_entry_t adapters[GW_PROTO_MAX_ADAPTERS];
    size_t adapter_count;
    gw_proto_router_stats_t stats;
    bool initialized;
    bool healthy;

    gw_mcp_server_t *mcp_server;
    gw_a2a_handler_t *a2a_handler;
    gw_llm_api_t *llm_api;
};

static int gw_proto_router_shutdown(gw_proto_router_t *router);

static gw_proto_detect_result_t detect_from_content_type(const char *content_type)
{
    if (!content_type)
        return GW_PROTO_DETECT_UNKNOWN;
    if (strstr(content_type, "application/json")) {
        return GW_PROTO_DETECT_JSONRPC;
    }
    if (strstr(content_type, "text/event-stream")) {
        return GW_PROTO_DETECT_LLM_API;
    }
    return GW_PROTO_DETECT_UNKNOWN;
}

static gw_proto_detect_result_t detect_from_path(const char *path)
{
    if (!path)
        return GW_PROTO_DETECT_UNKNOWN;
    if (strncmp(path, "/mcp", 4) == 0)
        return GW_PROTO_DETECT_MCP;
    if (strncmp(path, "/a2a", 4) == 0)
        return GW_PROTO_DETECT_A2A;
    if (strstr(path, "/v1/"))
        return GW_PROTO_DETECT_LLM_API;
    if (strncmp(path, "/api/", 5) == 0)
        return GW_PROTO_DETECT_JSONRPC;
    return GW_PROTO_DETECT_UNKNOWN;
}

static gw_proto_detect_result_t map_detect_type(protocol_type_t type)
{
    switch (type) {
    case AIRY_PROTOCOL_MCP:
        return GW_PROTO_DETECT_MCP;
    case AIRY_PROTOCOL_A2A:
        return GW_PROTO_DETECT_A2A;
    case AIRY_PROTOCOL_JSON_RPC:
        return GW_PROTO_DETECT_JSONRPC;
    default:
        if (proto_is_llm(type))
            return GW_PROTO_DETECT_LLM_API;
        return GW_PROTO_DETECT_UNKNOWN;
    }
}

static gw_proto_detect_result_t detect_from_body(const char *body)
{
    if (!body)
        return GW_PROTO_DETECT_UNKNOWN;

    /* 0.1.19 unit 60: the unified mechanism-core probe is the single
     * body classifier shared with the gateway front end; this layer only
     * maps the canonical type onto the router-local enum. */
    return map_detect_type(proto_detect_body(body, strlen(body)));
}

gw_proto_router_t *gw_proto_router_create(void)
{
    gw_proto_router_t *router = (gw_proto_router_t *)AIRY_CALLOC(1, sizeof(gw_proto_router_t));
    if (!router) {
        AIRY_ERROR_NULL(AIRY_ERR_UNKNOWN, "validation failed");
    }
    router->adapter_count = 0;
    router->initialized = false;
    router->healthy = false;
    router->mcp_server = NULL;
    router->a2a_handler = NULL;
    router->llm_api = NULL;
    return router;
}

void gw_proto_router_destroy(gw_proto_router_t *router)
{
    if (!router)
        return;
    if (router->initialized) {
        gw_proto_router_shutdown(router);
    }
    AIRY_FREE(router);
}

int gw_proto_router_init(gw_proto_router_t *router)
{
    if (!router)
        return AIRY_ERR_INVALID_PARAM;
    if (router->initialized)
        return 0;

    AIRY_MEMSET(&router->stats, 0, sizeof(router->stats));

    gw_mcp_server_config_t mcp_cfg = GW_MCP_SERVER_CONFIG_DEFAULTS;
    router->mcp_server = gw_mcp_server_create(&mcp_cfg);
    if (router->mcp_server) {
        gw_mcp_server_init(router->mcp_server);
        gw_proto_router_register(router, GW_PROTO_DETECT_MCP,
                                 gw_mcp_server_get_handler(router->mcp_server),
                                 gw_mcp_server_get_handler_data(router->mcp_server));
    }

    gw_a2a_handler_config_t a2a_cfg = GW_A2A_HANDLER_CONFIG_DEFAULTS;
    router->a2a_handler = gw_a2a_handler_create(&a2a_cfg);
    if (router->a2a_handler) {
        gw_a2a_handler_init(router->a2a_handler);
        gw_proto_router_register(router, GW_PROTO_DETECT_A2A,
                                 gw_a2a_handler_get_handler(router->a2a_handler),
                                 gw_a2a_handler_get_handler_data(router->a2a_handler));
    }

    gw_llm_api_config_t llm_cfg = GW_LLM_API_CONFIG_DEFAULTS;
    router->llm_api = gw_llm_api_create(&llm_cfg);
    if (router->llm_api) {
        gw_llm_api_init(router->llm_api);
        gw_proto_router_register(router, GW_PROTO_DETECT_LLM_API,
                                 gw_llm_get_handler(router->llm_api),
                                 gw_llm_get_hdata(router->llm_api));
    }

    router->initialized = true;
    router->healthy = true;
    return 0;
}

static int gw_proto_router_shutdown(gw_proto_router_t *router)
{
    if (!router || !router->initialized)
        return AIRY_ERR_INVALID_PARAM;

    if (router->mcp_server) {
        gw_mcp_server_shutdown(router->mcp_server);
        gw_mcp_server_destroy(router->mcp_server);
        router->mcp_server = NULL;
    }
    if (router->a2a_handler) {
        gw_a2a_handler_shutdown(router->a2a_handler);
        gw_a2a_handler_destroy(router->a2a_handler);
        router->a2a_handler = NULL;
    }
    if (router->llm_api) {
        gw_llm_api_shutdown(router->llm_api);
        gw_llm_api_destroy(router->llm_api);
        router->llm_api = NULL;
    }

    router->adapter_count = 0;
    router->initialized = false;
    router->healthy = false;
    return 0;
}

gw_proto_detect_result_t gw_proto_detect(const char *content_type, const char *path,
                                         const char *body)
{
    gw_proto_detect_result_t result;

    result = detect_from_path(path);
    if (result != GW_PROTO_DETECT_UNKNOWN)
        return result;

    result = detect_from_content_type(content_type);
    if (result != GW_PROTO_DETECT_UNKNOWN)
        return result;

    result = detect_from_body(body);
    return result;
}

int gw_proto_router_register(gw_proto_router_t *router, gw_proto_detect_result_t proto_type,
                             gw_proto_request_handler_t handler, void *user_data)
{
    if (!router || !handler)
        return AIRY_ERR_INVALID_PARAM;
    if (router->adapter_count >= GW_PROTO_MAX_ADAPTERS)
        return AIRY_ERR_OVERFLOW;

    for (size_t i = 0; i < router->adapter_count; i++) {
        if (router->adapters[i].proto_type == proto_type) {
            router->adapters[i].handler = handler;
            router->adapters[i].user_data = user_data;
            router->adapters[i].registered = true;
            return 0;
        }
    }

    gw_proto_adapter_entry_t *entry = &router->adapters[router->adapter_count];
    entry->proto_type = proto_type;
    entry->handler = handler;
    entry->user_data = user_data;
    entry->registered = true;
    router->adapter_count++;
    return 0;
}

static gw_proto_request_handler_t find_handler(gw_proto_router_t *router,
                                               gw_proto_detect_result_t proto_type,
                                               void **out_user_data)
{
    for (size_t i = 0; i < router->adapter_count; i++) {
        if (router->adapters[i].proto_type == proto_type && router->adapters[i].registered) {
            if (out_user_data)
                *out_user_data = router->adapters[i].user_data;
            return router->adapters[i].handler;
        }
    }
    AIRY_ERROR_NULL(AIRY_ERR_OVERFLOW, "limit exceeded");
}

static void record_proto_stats(gw_proto_router_stats_t *stats, gw_proto_detect_result_t proto_type)
{
    const char *name = proto_type_name(proto_type);
    (void)name;

    switch (proto_type) {
    case GW_PROTO_DETECT_MCP:
        stats->mcp_requests++;
        AIRY_LOG_DEBUG("protocol=%-8s count=%llu (mcp_requests)", name,
                  (unsigned long long)stats->mcp_requests);
        break;
    case GW_PROTO_DETECT_A2A:
        stats->a2a_requests++;
        AIRY_LOG_DEBUG("protocol=%-8s count=%llu (a2a_requests)", name,
                  (unsigned long long)stats->a2a_requests);
        break;
    case GW_PROTO_DETECT_LLM_API:
        stats->llm_requests++;
        AIRY_LOG_DEBUG("protocol=%-8s count=%llu (llm_requests)", name,
                  (unsigned long long)stats->llm_requests);
        break;
    case GW_PROTO_DETECT_JSONRPC:
        stats->jsonrpc_requests++;
        AIRY_LOG_DEBUG("protocol=%-8s count=%llu (jsonrpc_requests)", name,
                  (unsigned long long)stats->jsonrpc_requests);
        break;
    default:
        stats->unknown_requests++;
        AIRY_LOG_DEBUG("protocol=%-8s count=%llu (unknown_requests)", name,
                  (unsigned long long)stats->unknown_requests);
        break;
    }
}

int gw_proto_router_route(gw_proto_router_t *router, gw_proto_detect_result_t proto_type,
                          const char *method, const char *path, const char *body_json,
                          char **response_json)
{
    if (!router || !method || !response_json)
        return AIRY_ERR_INVALID_PARAM;

    router->stats.total_requests++;

    void *user_data = NULL;
    gw_proto_request_handler_t handler = find_handler(router, proto_type, &user_data);

    if (!handler) {
        AIRY_LOG_WARN("no handler found for protocol type=%d, route_errors=%llu", proto_type,
                 (unsigned long long)router->stats.route_errors);
        router->stats.route_errors++;
        record_proto_stats(&router->stats, proto_type);
        return AIRY_ERR_NOT_FOUND;
    }

    record_proto_stats(&router->stats, proto_type);

    int result = handler(method, path, body_json, response_json, user_data);
    if (result != 0) {
        AIRY_LOG_WARN("handler returned error: proto_type=%d, result=%d, path=%s", proto_type, result,
                 path ? path : "(null)");
        router->stats.route_errors++;
    }
    return result;
}

int gw_proto_router_get_stats(gw_proto_router_t *router, gw_proto_router_stats_t *stats)
{
    if (!router || !stats)
        return AIRY_ERR_INVALID_PARAM;
    *stats = router->stats;
    return 0;
}

gw_mcp_server_t *gw_proto_router_get_mcp(gw_proto_router_t *router)
{
    return router ? router->mcp_server : NULL;
}

gw_llm_api_t *gw_proto_get_llm(gw_proto_router_t *router)
{
    return router ? router->llm_api : NULL;
}

gw_a2a_handler_t *gw_proto_router_get_a2a(gw_proto_router_t *router)
{
    return router ? router->a2a_handler : NULL;
}
