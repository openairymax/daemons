// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file test_mcp_tools_schema.c
 * @brief 内置 MCP 工具 schema 一致性门禁（0.1.6 S-6 收敛）
 *
 * 验证 gateway 暴露给 MCP 客户端的内置工具集（gw_biz_mcp_register_tools）
 * 与 gateway_biz_tools.c 的权威 schema 表逐字节一致，杜绝与 tool_d
 * 参数定义的隐性漂移（S-6：fs_list.path 曾 required/optional 不一致）。
 *
 * 权威 schema 与 tool_d/src/service_builtin.c 参数定义对应：
 *   fs_read    path(req)          fs_write   path(req) content(req)
 *   fs_list    path(opt)          shell_run  command(req)
 *   web_fetch  url(req)           fs_glob    pattern(req) base(opt)
 *   fs_grep    pattern(req) path/glob/max_results(opt)
 *   fs_edit    path(req) old(req) new(req) count(opt)
 *   web_search query(req) max_results(opt)
 */

#include "gateway_biz_internal.h"
#include "gateway_mcp_server.h"
#include "gateway_business_handler.h"

#include <cjson/cJSON.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 权威工具表（name, inputSchema；与 gateway_biz_tools.c SSoT）---- */
typedef struct {
    const char *name;
    const char *schema;
} gw_tool_authority_t;

static const gw_tool_authority_t AUTHORITY_TOOLS[] = {
    {"fs_read",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"},
    {"fs_write",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}"},
    {"fs_list",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}"},
    {"shell_run",
     "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}"},
    {"web_fetch",
     "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}"},
    {"fs_glob",
     "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"base\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}"},
    {"fs_grep",
     "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"glob\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},\"required\":[\"pattern\"]}"},
    {"fs_edit",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"},\"count\":{\"type\":\"integer\"}},\"required\":[\"path\",\"old\",\"new\"]}"},
    {"web_search",
     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},\"required\":[\"query\"]}"},
};
#define AUTHORITY_COUNT \
    (sizeof(AUTHORITY_TOOLS) / sizeof(AUTHORITY_TOOLS[0]))

static void test_builtin_tools_schema(void)
{
    printf("test_builtin_tools_schema...\n");

    gw_mcp_server_config_t cfg = GW_MCP_SERVER_CONFIG_DEFAULTS;
    gw_mcp_server_t *mcp = gw_mcp_server_create(&cfg);
    assert(mcp != NULL);

    int rc = gw_biz_mcp_register_tools(mcp, NULL);
    assert(rc == 0);

    char *resp = NULL;
    rc = gw_mcp_server_handle_jsonrpc(mcp, "tools/list", "{}", &resp);
    assert(rc == 0);
    assert(resp != NULL);

    cJSON *root = cJSON_Parse(resp);
    assert(root != NULL);
    cJSON *res = cJSON_GetObjectItem(root, "result");
    assert(res != NULL);
    cJSON *tools = cJSON_GetObjectItem(res, "tools");
    assert(tools != NULL);
    int n = cJSON_GetArraySize(tools);
    assert(n == (int)AUTHORITY_COUNT);

    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(tools, i);
        const char *name = cJSON_GetObjectItem(t, "name")->valuestring;
        cJSON *schema_item = cJSON_GetObjectItem(t, "inputSchema");
        char *schema_str = cJSON_PrintUnformatted(schema_item);
        assert(schema_str != NULL);

        const gw_tool_authority_t *exp = NULL;
        for (size_t j = 0; j < AUTHORITY_COUNT; j++) {
            if (strcmp(AUTHORITY_TOOLS[j].name, name) == 0) {
                exp = &AUTHORITY_TOOLS[j];
                break;
            }
        }
        assert(exp != NULL);
        /* 权威 schema 与运行时 inputSchema 的 JSON 结构逐字节一致 */
        cJSON *exp_json = cJSON_Parse(exp->schema);
        char *exp_str = cJSON_PrintUnformatted(exp_json);
        assert(exp_str != NULL);
        if (strcmp(exp_str, schema_str) != 0) {
            fprintf(stderr, "schema mismatch for %s:\n  exp: %s\n  got: %s\n", name,
                    exp_str, schema_str);
        }
        assert(strcmp(exp_str, schema_str) == 0);

        cJSON_free(exp_str);
        cJSON_free(exp_json);
        cJSON_free(schema_str);
    }

    cJSON_Delete(root);
    free(resp);
    gw_mcp_server_destroy(mcp);
    printf("  ok (9 tools, schema 与权威表一致)\n");
}

int main(void)
{
    test_builtin_tools_schema();
    printf("\nAll gateway MCP tools schema tests passed.\n");
    return 0;
}
