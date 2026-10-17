// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file gw_mcpclients.c
 * @brief gateway_d 外部 MCP client 桥域（P2-4，AIRY_MCP_CLIENTS env）。
 *
 * POSIX-only：protocols 库在 WIN32 下整块裁剪 mcp_client_*
 * （mcp_client_internal.h 用 pid_t 且 stdio 传输依赖 fork/exec，见
 * protocols/CMakeLists.txt PROTOCOLS_ENABLE_MCP_TRANSPORT WIN32 恒
 * OFF）。Windows 构建沿用同一决策，本域整体编译排除（#112 实证
 * main.obj 9×LNK2001 mcp_client_*），CMake 侧另以条件装配对齐。
 */

#include "gateway_d_internal.h"

#ifndef _WIN32

#include "gateway_mcp_server.h"
#include "mcp_client.h"
#include "svc_logger.h"
#include "airy_memory.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GW_MCP_CLIENTS_MAX 32
#define GW_MCP_CLIENT_NAME_LEN 64

/**
 * @brief Runtime context of a single external MCP server
 * @note All external tools of one config entry share the same ctx (user_data);
 *       exec_fn strips the prefix from "<client>_<tool>" using ctx->name.
 */
typedef struct {
    char name[GW_MCP_CLIENT_NAME_LEN];
    mcp_client_t *client;
} gw_mcp_client_ctx_t;

static gw_mcp_client_ctx_t g_mcp_clients[GW_MCP_CLIENTS_MAX];
static size_t g_mcp_client_count = 0;

/**
 * @brief External tool forwarding exec_fn: <client>_<tool> -> external server
 *        tools/call
 *
 * Result handling: the external server returns a complete JSON-RPC response
 * ("result JSON returned as-is"); the first text content's JSON string is
 * extracted and handed back to the gateway_mcp_server layer (whose tools/call
 * response embeds it as "text":%s), keeping MCP-compliant output.
 */
static int gw_mcp_client_tool_exec(const char *tool_name, const char *arguments_json,
                                   char **result_json, void *user_data)
{
    gw_mcp_client_ctx_t *ctx = (gw_mcp_client_ctx_t *)user_data;
    *result_json = NULL;
    if (!ctx || !ctx->client || !tool_name) {
        *result_json = AIRY_STRDUP("\"invalid external mcp tool request\"");
        return -1;
    }
    size_t prefix_len = strlen(ctx->name);
    if (strncmp(tool_name, ctx->name, prefix_len) != 0 || tool_name[prefix_len] != '_') {
        SVC_LOG_WARN("P2-4: tool '%s' prefix mismatch with client '%s'", tool_name, ctx->name);
        *result_json = AIRY_STRDUP("\"external tool name prefix mismatch\"");
        return -1;
    }
    const char *orig_name = tool_name + prefix_len + 1;

    char *resp = NULL;
    int rc = mcp_client_call_tool(ctx->client, orig_name, arguments_json, &resp);
    if (rc != 0 || !resp) {
        SVC_LOG_WARN("P2-4: tool call '%s' via client '%s' failed (rc=%d)", orig_name, ctx->name,
                     rc);
        *result_json = AIRY_STRDUP("\"external mcp tool call failed\"");
        return -1;
    }
    rc = mcp_client_extract_text(resp, result_json);
    AIRY_FREE(resp);
    if (rc != 0 || !*result_json) {
        *result_json = AIRY_STRDUP("\"failed to parse external mcp tool response\"");
        return -1;
    }
    return 0;
}

/**
 * @brief Disconnect all external MCP servers and reset (called from the main
 *        exit path)
 */
void gw_mcp_client_cleanup(void)
{
    for (size_t i = 0; i < g_mcp_client_count; i++) {
        if (g_mcp_clients[i].client) {
            SVC_LOG_INFO("P2-4: disconnecting external MCP client '%s'", g_mcp_clients[i].name);
            mcp_client_disconnect(g_mcp_clients[i].client);
            g_mcp_clients[i].client = NULL;
        }
    }
    g_mcp_client_count = 0;
}

/**
 * @brief Read the AIRY_MCP_CLIENTS env var and connect external MCP servers
 *
 * Config format (JSON array):
 *
 * [{"name":"filesystem","command":"npx","args":["-y",
 *   "@modelcontextprotocol/server-filesystem","/tmp"]}]
 *   [{"name":"remote","type":"http","url":"http://127.0.0.1:3001/mcp"}]
 *
 * For each entry: connect -> tools/list -> register into the gateway MCP tool
 * table with the "<client>_<tool>" prefix (exec_fn forwards external calls).
 * Connect/fetch failures only warn and do not block gateway startup.
 */
void gw_mcp_clients_setup(gw_mcp_server_t *mcp)
{
    if (!mcp)
        return;
    const char *env = getenv("AIRY_MCP_CLIENTS");
    if (!env || !env[0]) {
        SVC_LOG_INFO("P2-4: AIRY_MCP_CLIENTS not set, external MCP clients disabled");
        return;
    }

    cJSON *root = cJSON_Parse(env);
    if (!root || !cJSON_IsArray(root)) {
        SVC_LOG_WARN("P2-4: AIRY_MCP_CLIENTS is not a valid JSON array, ignored");
        if (root)
            cJSON_Delete(root);
        return;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root)
    {
        if (g_mcp_client_count >= GW_MCP_CLIENTS_MAX)
            break;
        cJSON *jname = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(jname) || !jname->valuestring || !jname->valuestring[0]) {
            SVC_LOG_WARN("P2-4: mcp client entry missing 'name', skipped");
            continue;
        }

        gw_mcp_client_ctx_t *ctx = &g_mcp_clients[g_mcp_client_count];
        AIRY_STRNCPY_TERM(ctx->name, jname->valuestring, sizeof(ctx->name));

        const char *transport = "stdio";
        cJSON *jtype = cJSON_GetObjectItem(item, "type");
        if (cJSON_IsString(jtype) && jtype->valuestring)
            transport = jtype->valuestring;

        if (strcmp(transport, "http") == 0) {

            cJSON *jurl = cJSON_GetObjectItem(item, "url");
            if (!cJSON_IsString(jurl) || !jurl->valuestring || !jurl->valuestring[0]) {
                SVC_LOG_WARN("P2-4: mcp client '%s' type=http requires 'url', skipped", ctx->name);
                continue;
            }
            ctx->client = mcp_client_connect_http(ctx->name, jurl->valuestring);
        } else {

            cJSON *jcmd = cJSON_GetObjectItem(item, "command");
            if (!cJSON_IsString(jcmd) || !jcmd->valuestring || !jcmd->valuestring[0]) {
                SVC_LOG_WARN("P2-4: mcp client '%s' requires 'command', skipped", ctx->name);
                continue;
            }
            char *argv_arr[64];
            int argc = 0;
            argv_arr[argc++] = jcmd->valuestring;
            cJSON *jargs = cJSON_GetObjectItem(item, "args");
            if (cJSON_IsArray(jargs)) {
                cJSON *ja = NULL;
                cJSON_ArrayForEach(ja, jargs)
                {
                    if (argc >= 63)
                        break;
                    if (cJSON_IsString(ja) && ja->valuestring)
                        argv_arr[argc++] = ja->valuestring;
                }
            }
            argv_arr[argc] = NULL;
            ctx->client = mcp_client_connect_stdio(ctx->name, jcmd->valuestring, argv_arr);
        }

        if (!ctx->client) {
            SVC_LOG_WARN("P2-4: failed to connect external MCP server '%s' (transport=%s), "
                         "gateway continues",
                         ctx->name, transport);
            continue;
        }

        mcp_client_tool_list_t list;
        AIRY_MEMSET(&list, 0, sizeof(list));
        int rc = mcp_client_list_tools(ctx->client, &list);
        if (rc != 0) {
            SVC_LOG_WARN("P2-4: failed to list tools from '%s' (rc=%d), disconnected", ctx->name,
                         rc);
            mcp_client_disconnect(ctx->client);
            ctx->client = NULL;
            continue;
        }

        size_t registered = 0;
        for (size_t i = 0; i < list.count; i++) {
            char full_name[GW_MCP_CLIENT_NAME_LEN + 128];
            snprintf(full_name, sizeof(full_name), "%s_%s", ctx->name,
                     list.tools[i].name ? list.tools[i].name : "");
            int rrc = gw_mcp_server_register_tool(
                mcp, full_name, list.tools[i].description ? list.tools[i].description : "",
                list.tools[i].input_schema_json ? list.tools[i].input_schema_json : "{}",
                gw_mcp_client_tool_exec, ctx);
            if (rrc == 0) {
                registered++;
            } else {
                SVC_LOG_WARN("P2-4: failed to register external tool '%s' (rc=%d)", full_name, rrc);
            }
        }
        mcp_client_tool_list_free(&list);

        if (registered == 0) {
            SVC_LOG_WARN("P2-4: external MCP server '%s' exposes no usable tools, "
                         "disconnected",
                         ctx->name);
            mcp_client_disconnect(ctx->client);
            ctx->client = NULL;
            continue;
        }
        SVC_LOG_INFO("P2-4: external MCP client '%s' connected (transport=%s), "
                     "%zu tools registered",
                     ctx->name, transport, registered);
        g_mcp_client_count++;
    }

    cJSON_Delete(root);
}

#endif /* !_WIN32 */
