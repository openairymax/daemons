/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gateway_biz_tools.c
 * @brief Gateway 内置 MCP 工具注册域（SSoT，2026-08-30 S-6 收敛）。
 *
 * 内置工具的 JSON schema 在网关侧唯一权威于此文件，与 tool_d 的
 * service_builtin.c 参数定义保持一致（见下方 GW_*_SCHEMA 注释的
 * 参数对应关系）；gateway 主流程与一致性测试均调用
 * gw_biz_mcp_register_tools()，杜绝双份手写内联漂移。
 *
 * S-6 漂移修复：fs_list.path 原在 gateway 声明 required，而 tool_d
 * 校验器对 optional 参数缺失放行（validator.c：仅缺失 required 才
 * 拒绝）且 builtin 实现省略时默认 "."——现统一为 optional。
 */

#include "gateway_mcp_server.h"
#include "gateway_business_handler.h"
#include "gateway_biz_internal.h"

/* ---- 内置工具 JSON schema（SSoT）----
 * 参数对应 tool_d/src/service_builtin.c 的 tool_param_t 定义：
 *   fs_read    path(string,req)
 *   fs_write   path(string,req) content(string,req)
 *   fs_list    path(string,opt——默认 ".")
 *   shell_run  command(string,req)
 *   web_fetch  url(string,req)
 *   fs_glob    pattern(string,req) base(string,opt)
 *   fs_grep    pattern(string,req) path(string,opt) glob(string,opt) max_results(int,opt)
 *   fs_edit    path(string,req) old(string,req) new(string,req) count(int,opt)
 *   web_search query(string,req) max_results(int,opt) */

#define GW_FS_READ_SCHEMA \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"
#define GW_FS_WRITE_SCHEMA \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}"
#define GW_FS_LIST_SCHEMA \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}"
#define GW_SHELL_RUN_SCHEMA \
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}"
#define GW_WEB_FETCH_SCHEMA \
    "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}"
#define GW_FS_GLOB_SCHEMA \
    "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"base\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}"
#define GW_FS_GREP_SCHEMA \
    "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"glob\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},\"required\":[\"pattern\"]}"
#define GW_FS_EDIT_SCHEMA \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"},\"count\":{\"type\":\"integer\"}},\"required\":[\"path\",\"old\",\"new\"]}"
#define GW_WEB_SEARCH_SCHEMA \
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},\"required\":[\"query\"]}"

/* 注册 gateway 暴露给 MCP 客户端的内置工具集。user_data 透传执行回调
 * （gateway_business_ctx_t）。返回失败数（0 = 全部注册成功）。 */
int gw_biz_mcp_register_tools(gw_mcp_server_t *mcp, void *user_data)
{
    int failed = 0;

    if (!mcp) {
        return -1;
    }

    if (gw_mcp_server_register_tool(mcp, "fs_read",
                                    "Read a file's content from the local filesystem",
                                    GW_FS_READ_SCHEMA, gw_biz_tool_exec, user_data) != 0) {
        failed++;
    }
    if (gw_mcp_server_register_tool(mcp, "fs_write",
                                    "Write content to a local file (creates or overwrites)",
                                    GW_FS_WRITE_SCHEMA, gw_biz_tool_exec, user_data) != 0) {
        failed++;
    }
    if (gw_mcp_server_register_tool(mcp, "fs_list",
                                    "List entries of a local directory (JSON array)",
                                    GW_FS_LIST_SCHEMA, gw_biz_tool_exec, user_data) != 0) {
        failed++;
    }
    if (gw_mcp_server_register_tool(mcp, "shell_run",
                                    "Execute a shell command and capture its output",
                                    GW_SHELL_RUN_SCHEMA, gw_biz_tool_exec, user_data) != 0) {
        failed++;
    }
    if (gw_mcp_server_register_tool(mcp, "web_fetch",
                                    "Fetch a web page over HTTP(S) and return its body text",
                                    GW_WEB_FETCH_SCHEMA, gw_biz_tool_exec, user_data) != 0) {
        failed++;
    }
    if (gw_mcp_server_register_tool(mcp, "fs_glob",
                                    "List files matching a glob pattern (supports * ? and **)",
                                    GW_FS_GLOB_SCHEMA, gw_biz_tool_exec, user_data) != 0) {
        failed++;
    }
    if (gw_mcp_server_register_tool(mcp, "fs_grep",
                                    "Search file contents with a regular expression (relpath:line:text)",
                                    GW_FS_GREP_SCHEMA, gw_biz_tool_exec, user_data) != 0) {
        failed++;
    }
    if (gw_mcp_server_register_tool(mcp, "fs_edit",
                                    "Replace an exact string in a file (search-and-replace edit)",
                                    GW_FS_EDIT_SCHEMA, gw_biz_tool_exec, user_data) != 0) {
        failed++;
    }
    if (gw_mcp_server_register_tool(mcp, "web_search",
                                    "Search the web (DuckDuckGo) and return ranked results",
                                    GW_WEB_SEARCH_SCHEMA, gw_biz_tool_exec, user_data) != 0) {
        failed++;
    }

    return failed;
}
