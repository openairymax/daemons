// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 *
 * @file test_protocol_router.c
 * @brief 网关协议路由器统计计数验证测试
 *
 * 验证 record_proto_stats 重构后，gw_proto_router_route 的统计逻辑正确。
 * 覆盖场景：
 *   - 5 种协议类型（MCP / A2A / OpenAI / JSON-RPC / Unknown）在"handler 存在"和"handler 不存在"
 *     两条路径下的统计计数
 *   - total_requests / route_errors 全局计数器
 */

#include "gateway_protocol_router.h"
#include "http_gateway.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"

/* ==================== Mock handler ==================== */
static int mock_handler(const char *method, const char *path, const char *body_json,
                        char **response_json, void *user_data)
{
    (void)method;
    (void)path;
    (void)body_json;
    (void)user_data;

    if (response_json)
        *response_json = NULL;
    return 0;
}

static const int TEST_PROTO_TYPES[] = {GW_PROTO_DETECT_MCP, /* 1 */
                                       GW_PROTO_DETECT_A2A, /* 2 */
                                       GW_PROTO_DETECT_OPENAI, /* 3 */
                                       GW_PROTO_DETECT_JSONRPC, /* 4 */
                                       GW_PROTO_DETECT_UNKNOWN};
static const int NUM_PROTO_TYPES = 5;

static const char *proto_label(gw_proto_detect_result_t t)
{
    switch (t) {
    case GW_PROTO_DETECT_MCP:
        return "MCP";
    case GW_PROTO_DETECT_A2A:
        return "A2A";
    case GW_PROTO_DETECT_OPENAI:
        return "OpenAI";
    case GW_PROTO_DETECT_JSONRPC:
        return "JSON-RPC";
    default:
        return "Unknown";
    }
}

/**
 * @brief Test scenario A: statistics counting when the handler exists
 *
 * Register a mock handler -> call route for each protocol -> verify each
 * counter is 1
 */
static void test_stats_with_handler(void)
{
    printf("\n--- Scenario A: route with handler ---\n");

    gw_proto_router_t *router = gw_proto_router_create();
    assert(router != NULL);

    for (int i = 0; i < NUM_PROTO_TYPES; i++) {
        gw_proto_detect_result_t pt = (gw_proto_detect_result_t)TEST_PROTO_TYPES[i];
        int rc = gw_proto_router_register(router, pt, mock_handler, NULL);
        assert(rc == 0);
        (void)rc;
        printf("  registered handler for %s\n", proto_label(pt));
    }

    for (int i = 0; i < NUM_PROTO_TYPES; i++) {
        gw_proto_detect_result_t pt = (gw_proto_detect_result_t)TEST_PROTO_TYPES[i];
        char *resp = NULL;
        int rc = gw_proto_router_route(router, pt, "GET", "/test", "{}", &resp);

        printf("  route(%s) => %d\n", proto_label(pt), rc);
    }

    gw_proto_router_stats_t stats;
    int rc = gw_proto_router_get_stats(router, &stats);
    assert(rc == 0);
    (void)rc;

    printf("  total_requests=%llu\n", (unsigned long long)stats.total_requests);
    assert(stats.total_requests == (uint64_t)NUM_PROTO_TYPES && "total_requests should be 5");

    assert(stats.mcp_requests == 1 && "mcp_requests should be 1");
    assert(stats.a2a_requests == 1 && "a2a_requests should be 1");
    assert(stats.openai_requests == 1 && "openai_requests should be 1");
    assert(stats.jsonrpc_requests == 1 && "jsonrpc_requests should be 1");
    assert(stats.unknown_requests == 1 && "unknown_requests should be 1");
    assert(stats.route_errors == 0 && "route_errors should be 0 (handler found)");

    printf("  >>> PASSED\n");

    gw_proto_router_destroy(router);
}

/**
 * @brief Test scenario B: statistics counting when the handler does not exist
 *
 * Register no handler -> each route should take the if(!handler) branch ->
 * route_errors increments + the proto counter increments
 */
static void test_stats_without_handler(void)
{
    printf("\n--- Scenario B: route without handler ---\n");

    gw_proto_router_t *router = gw_proto_router_create();
    assert(router != NULL);

    const int N = 3;
    for (int i = 0; i < NUM_PROTO_TYPES; i++) {
        gw_proto_detect_result_t pt = (gw_proto_detect_result_t)TEST_PROTO_TYPES[i];
        for (int j = 0; j < N; j++) {
            char *resp = NULL;
            int rc = gw_proto_router_route(router, pt, "GET", "/test", "{}", &resp);
            assert(rc == AIRY_ERR_NOT_FOUND);
            (void)rc;
        }
    }

    gw_proto_router_stats_t stats;
    gw_proto_router_get_stats(router, &stats);

    printf("  total_requests=%llu\n", (unsigned long long)stats.total_requests);
    printf("  route_errors=%llu\n", (unsigned long long)stats.route_errors);

    assert(stats.total_requests == (uint64_t)(NUM_PROTO_TYPES * N) && "total=15");
    assert(stats.route_errors == (uint64_t)(NUM_PROTO_TYPES * N) && "errors=15");

    assert(stats.mcp_requests == (uint64_t)N && "mcp=3");
    assert(stats.a2a_requests == (uint64_t)N && "a2a=3");
    assert(stats.openai_requests == (uint64_t)N && "openai=3");
    assert(stats.jsonrpc_requests == (uint64_t)N && "jsonrpc=3");
    assert(stats.unknown_requests == (uint64_t)N && "unknown=3");

    printf("  >>> PASSED\n");

    gw_proto_router_destroy(router);
}

/**
 * @brief Test scenario C: mixed — handler exists + does not exist
 *
 * Register only MCP and A2A handlers, verifying:
 *   MCP/A2A take the normal path, route_errors=0
 *   OpenAI/JSONRPC/Unknown take the 404 path, route_errors increments
 */
static void test_stats_mixed(void)
{
    printf("\n--- Scenario C: mixed (MCP+A2A registered, others unregistered) ---\n");

    gw_proto_router_t *router = gw_proto_router_create();
    assert(router != NULL);

    gw_proto_router_register(router, GW_PROTO_DETECT_MCP, mock_handler, NULL);
    gw_proto_router_register(router, GW_PROTO_DETECT_A2A, mock_handler, NULL);

    for (int i = 0; i < NUM_PROTO_TYPES; i++) {
        gw_proto_detect_result_t pt = (gw_proto_detect_result_t)TEST_PROTO_TYPES[i];
        char *resp = NULL;
        gw_proto_router_route(router, pt, "GET", "/test", "{}", &resp);
    }

    gw_proto_router_stats_t stats;
    gw_proto_router_get_stats(router, &stats);

    printf("  mcp_requests=%llu  a2a_requests=%llu  openai_requests=%llu  "
           "jsonrpc_requests=%llu  unknown_requests=%llu\n",
           (unsigned long long)stats.mcp_requests, (unsigned long long)stats.a2a_requests,
           (unsigned long long)stats.openai_requests, (unsigned long long)stats.jsonrpc_requests,
           (unsigned long long)stats.unknown_requests);
    printf("  route_errors=%llu\n", (unsigned long long)stats.route_errors);

    assert(stats.total_requests == (uint64_t)NUM_PROTO_TYPES && "total=5");
    assert(stats.mcp_requests == 1);
    assert(stats.a2a_requests == 1);
    assert(stats.openai_requests == 1);
    assert(stats.jsonrpc_requests == 1);
    assert(stats.unknown_requests == 1);

    assert(stats.route_errors == 3 && "3 unregistered = 3 errors");

    printf("  >>> PASSED\n");

    gw_proto_router_destroy(router);
}

static void test_invalid_params(void)
{
    printf("\n--- Scenario D: invalid params ---\n");

    gw_proto_router_t *router = gw_proto_router_create();
    assert(router != NULL);

    int rc;

    rc = gw_proto_router_route(NULL, GW_PROTO_DETECT_MCP, "GET", "/t", "{}", NULL);
    assert(rc == AIRY_ERR_INVALID_PARAM);

    rc = gw_proto_router_route(router, GW_PROTO_DETECT_MCP, NULL, "/t", "{}", NULL);
    assert(rc == AIRY_ERR_INVALID_PARAM);
    (void)rc;

    printf("  >>> PASSED\n");

    gw_proto_router_destroy(router);
}

/**
 * @brief Test scenario E: path-aware detection for OpenAI endpoints
 *
 * Regression for the /v1/embeddings routing gap: the embeddings body
 * {"input":[...],"model":...} contains no "messages" field, so body-only
 * detection returns UNKNOWN. The HTTP transport now forwards the request
 * path (via gateway_http_request_t); gw_proto_detect must use it to
 * classify /v1 and /openai endpoints as OpenAI.
 */
static void test_detect_openai_by_path(void)
{
    printf("\n--- Scenario E: OpenAI detection via path ---\n");

    const char *embed_body = "{\"model\":\"test-embed\",\"input\":[\"hello\"]}";

    /* Body alone (no path): previously misrouted to JSON-RPC -32600 */
    gw_proto_detect_result_t no_path = gw_proto_detect(NULL, NULL, embed_body);
    assert(no_path != GW_PROTO_DETECT_OPENAI &&
           "embeddings body alone must not be detected as OpenAI");

    /* With the HTTP path preserved: must classify as OpenAI */
    gw_proto_detect_result_t by_path = gw_proto_detect(NULL, "/v1/embeddings", embed_body);
    assert(by_path == GW_PROTO_DETECT_OPENAI && "/v1/embeddings must be OpenAI");

    by_path = gw_proto_detect(NULL, "/openai/v1/embeddings", embed_body);
    assert(by_path == GW_PROTO_DETECT_OPENAI && "/openai/v1/embeddings must be OpenAI");

    by_path = gw_proto_detect(NULL, "/v1/chat/completions",
                              "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");
    assert(by_path == GW_PROTO_DETECT_OPENAI && "/v1/chat/completions must be OpenAI");

    /* JSON-RPC path must not be hijacked by the OpenAI rule */
    gw_proto_detect_result_t jrpc = gw_proto_detect(NULL, "/api", embed_body);
    assert(jrpc == GW_PROTO_DETECT_JSONRPC || jrpc == GW_PROTO_DETECT_UNKNOWN);

    printf("  >>> PASSED\n");
}

/**
 * @brief Test scenario F: gateway_http_request_t magic recognition
 *
 * The HTTP transport wraps the body in gateway_http_request_t. Verify the
 * magic byte pattern is distinct from plain JSON bodies (which start with
 * '{'/'['), so gateway_protocol_entry can disambiguate the two shapes.
 */
static void test_http_request_magic(void)
{
    printf("\n--- Scenario F: HTTP request context magic ---\n");

    char body[] = "{\"model\":\"m\"}";
    gateway_http_request_t req = {0};
    req.magic = GATEWAY_HTTP_REQUEST_MAGIC;
    req.method = "POST";
    req.path = "/v1/embeddings";
    req.body = body;
    req.body_len = strlen(body);

    const unsigned char *raw = (const unsigned char *)&req;
    assert(raw[0] != '{' && raw[0] != '[' &&
           "http request context must not look like a JSON body");

    uint32_t magic = 0;
    __builtin_memcpy(&magic, raw, sizeof(uint32_t));
    assert(magic == GATEWAY_HTTP_REQUEST_MAGIC && "magic must round-trip");

    printf("  >>> PASSED\n");
}

int main(void)
{
    printf("\n=== Gateway Protocol Router Stats Tests ===\n");
    printf("Build: %s %s\n", __DATE__, __TIME__);
    printf("Test file: %s\n\n", __FILE__);

    test_invalid_params();
    test_stats_with_handler();
    test_stats_without_handler();
    test_stats_mixed();
    test_detect_openai_by_path();
    test_http_request_magic();

    printf("\n=== All tests passed ===\n\n");
    return 0;
}