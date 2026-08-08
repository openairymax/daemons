/*
 * Copyright (C) 2026 SPHARX. All Rights Reserved.
 * SPDX-FileCopyrightText: 2026 SPHARX.
 * SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
 *
 * @file main.c
 * @brief Gateway守护进程主入口（遵循 daemon 模块统一规范）
 *
 * 规范遵循:
 * - ARCHITECTURAL_PRINCIPLES.md E-3 资源确定性(成对管理)
 * - ARCHITECTURAL_PRINCIPLES.md E-4 跨平台一致性(platform.h)
 * - ARCHITECTURAL_PRINCIPLES.md E-5 命名语义化(SVC_LOG_*)
 * - ARCHITECTURAL_PRINCIPLES.md E-6 错误可追溯(AIRY_ERR_*)
 */

#include "atomic_compat.h"
#include "daemon_bootstrap_sd.h"
#include "daemon_bootstrap_ipc.h"
#include "daemon_cupolas_bootstrap.h"
#include "gateway_service.h"
#include "gateway_business_handler.h"
#include "daemon_security.h"
#include "logging.h"
#include "daemon_platform_ext.h"
#include "svc_common.h"
#include "svc_config.h"
#include "svc_logger.h"
#include "error.h"
#include "airy_memory.h"

/* Phase 2: 协议适配接线（MCP/OpenAI/A2A 适配器 → 内部服务） */
#include "gateway_protocol_router.h"
#include "gateway_mcp_server.h"
#include "gateway_openai_compat.h"
#include "gateway_a2a_handler.h"

#ifdef AIRY_HAS_PROTOCOLS
#include "a2a_v03_adapter.h"
#include "mcp_v1_adapter.h"
#include "openai_enterprise_adapter.h"
#include "unified_protocol.h"
#endif

/* P2-4: MCP 客户端（消费外部 MCP server 工具）；cJSON 解析 AIRY_MCP_CLIENTS */
#include "mcp_client.h"
#include <cjson/cJSON.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h> /* umask */
#include <unistd.h>   /* write()：async-signal-safe 信号埋点 */
#endif

/* ==================== 全局状态 ==================== */

static gateway_service_t g_service = NULL;
static atomic_int g_running = 1;
static daemon_bootstrap_sd_t *g_bsd = NULL;
static daemon_bootstrap_ipc_t *g_bipc = NULL;
static gateway_business_ctx_t *g_biz_ctx = NULL; /* 业务处理器上下文 */
static gateway_entry_ctx_t g_entry_ctx;          /* 统一协议入口上下文 */
static gw_proto_router_t *g_proto_router = NULL; /* 协议路由器（MCP/OpenAI/A2A） */

/* ==================== P2-4: MCP 客户端（外部 MCP server 工具消费） ==================== */

#define GW_MCP_CLIENTS_MAX 32
#define GW_MCP_CLIENT_NAME_LEN 64

/**
 * @brief 单个外部 MCP server 的运行时上下文
 * @note 一个配置项的所有外部工具共享同一个 ctx（user_data），
 *       exec_fn 依据 ctx->name 从 "<client>_<tool>" 剥离前缀。
 */
typedef struct {
    char name[GW_MCP_CLIENT_NAME_LEN];
    mcp_client_t *client;
} gw_mcp_client_ctx_t;

static gw_mcp_client_ctx_t g_mcp_clients[GW_MCP_CLIENTS_MAX];
static size_t g_mcp_client_count = 0;

/**
 * @brief 外部工具转发 exec_fn：<client>_<tool> → 外部 server tools/call
 *
 * 结果处理：外部 server 返回完整 JSON-RPC 响应（"结果 JSON 原样返回"），
 * 提取首个 text content 的 JSON 字符串后交还 gateway_mcp_server 层
 * （其 tools/call 响应以 "text":%s 内嵌），保持 MCP 规范输出。
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
        SVC_LOG_WARN("P2-4: tool '%s' prefix mismatch with client '%s'", tool_name,
                     ctx->name);
        *result_json = AIRY_STRDUP("\"external tool name prefix mismatch\"");
        return -1;
    }
    const char *orig_name = tool_name + prefix_len + 1;

    char *resp = NULL;
    int rc = mcp_client_call_tool(ctx->client, orig_name, arguments_json, &resp);
    if (rc != 0 || !resp) {
        SVC_LOG_WARN("P2-4: tool call '%s' via client '%s' failed (rc=%d)", orig_name,
                     ctx->name, rc);
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
 * @brief 断开全部外部 MCP server 并复位（main 退出路径调用）
 */
static void gw_mcp_client_cleanup(void)
{
    for (size_t i = 0; i < g_mcp_client_count; i++) {
        if (g_mcp_clients[i].client) {
            SVC_LOG_INFO("P2-4: disconnecting external MCP client '%s'",
                         g_mcp_clients[i].name);
            mcp_client_disconnect(g_mcp_clients[i].client);
            g_mcp_clients[i].client = NULL;
        }
    }
    g_mcp_client_count = 0;
}

/**
 * @brief 读取 AIRY_MCP_CLIENTS 环境变量并连接外部 MCP server
 *
 * 配置格式（JSON 数组）：
 *   [{"name":"filesystem","command":"npx","args":["-y","@modelcontextprotocol/server-filesystem","/tmp"]}]
 *   [{"name":"remote","type":"http","url":"http://127.0.0.1:3001/mcp"}]
 *
 * 对每个配置项：连接 → tools/list → 以 "<client>_<tool>" 前缀注册进
 * gateway MCP 工具表（exec_fn 转发外部调用）。连接/拉取失败仅告警，
 * 不阻断 gateway 启动。
 */
static void gw_mcp_clients_setup(gw_mcp_server_t *mcp)
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
    cJSON_ArrayForEach(item, root) {
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
            /* http 传输：url 必填 */
            cJSON *jurl = cJSON_GetObjectItem(item, "url");
            if (!cJSON_IsString(jurl) || !jurl->valuestring || !jurl->valuestring[0]) {
                SVC_LOG_WARN("P2-4: mcp client '%s' type=http requires 'url', skipped",
                             ctx->name);
                continue;
            }
            ctx->client = mcp_client_connect_http(ctx->name, jurl->valuestring);
        } else {
            /* stdio 传输（默认）：command + args */
            cJSON *jcmd = cJSON_GetObjectItem(item, "command");
            if (!cJSON_IsString(jcmd) || !jcmd->valuestring || !jcmd->valuestring[0]) {
                SVC_LOG_WARN("P2-4: mcp client '%s' requires 'command', skipped",
                             ctx->name);
                continue;
            }
            char *argv_arr[64];
            int argc = 0;
            argv_arr[argc++] = jcmd->valuestring; /* 指向 cJSON 内部，connect 时深拷贝 */
            cJSON *jargs = cJSON_GetObjectItem(item, "args");
            if (cJSON_IsArray(jargs)) {
                cJSON *ja = NULL;
                cJSON_ArrayForEach(ja, jargs) {
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

        /* tools/list 拉取外部工具 */
        mcp_client_tool_list_t list;
        AIRY_MEMSET(&list, 0, sizeof(list));
        int rc = mcp_client_list_tools(ctx->client, &list);
        if (rc != 0) {
            SVC_LOG_WARN("P2-4: failed to list tools from '%s' (rc=%d), disconnected",
                         ctx->name, rc);
            mcp_client_disconnect(ctx->client);
            ctx->client = NULL;
            continue;
        }

        /* 注册外部工具：<client>_<tool>，exec_fn 转发 mcp_client_call_tool */
        size_t registered = 0;
        for (size_t i = 0; i < list.count; i++) {
            char full_name[GW_MCP_CLIENT_NAME_LEN + 128];
            snprintf(full_name, sizeof(full_name), "%s_%s", ctx->name,
                     list.tools[i].name ? list.tools[i].name : "");
            int rrc = gw_mcp_server_register_tool(
                mcp, full_name,
                list.tools[i].description ? list.tools[i].description : "",
                list.tools[i].input_schema_json ? list.tools[i].input_schema_json : "{}",
                gw_mcp_client_tool_exec, ctx);
            if (rrc == 0) {
                registered++;
            } else {
                SVC_LOG_WARN("P2-4: failed to register external tool '%s' (rc=%d)",
                             full_name, rrc);
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

/* ==================== Phase 3 R4: 协议层 ACL 默认规则 ==================== */

/**
 * @brief 注册外部协议默认 ACL 规则（fail-closed 需显式授权）
 *
 * 外部协议请求统一身份 "external"（见 gateway_business_handler.c）：
 *   - fs_read/fs_write/fs_list 基础工具默认允许（不误拦正常请求）
 *   - shell_run 默认允许（与基础工具一致）：gateway 已把 shell_run 作为工具
 *     schema 暴露给 LLM（GW_TOOLS_JSON），若默认拒绝则 LLM 一调用即被 ACL 拦截，
 *     导致工具链路整体不可用；如需关闭可设 AIRY_GATEWAY_ACL_ALLOW_SHELL=false
 *     显式拒绝（仅 "false"/"0" 视为关闭，其余取值默认放行）。
 *
 * 须在 daemon_security 初始化后（daemon_cupolas_init）调用。
 */
static void gw_acl_register_defaults(void)
{
    daemon_security_add_acl_rule("external", "fs_read", true);
    daemon_security_add_acl_rule("external", "fs_write", true);
    daemon_security_add_acl_rule("external", "fs_list", true);
    /* web_fetch 只读联网抓取，与 fs_read 同级信任，默认放行 */
    daemon_security_add_acl_rule("external", "web_fetch", true);
    /* fs_glob/fs_grep 只读检索、fs_edit 受控替换、web_search 只读联网搜索，
     * 与 fs_* 基础工具同级信任，默认放行（否则 LLM 一经 MCP 调用即被 ACL 拦截） */
    daemon_security_add_acl_rule("external", "fs_glob", true);
    daemon_security_add_acl_rule("external", "fs_grep", true);
    daemon_security_add_acl_rule("external", "fs_edit", true);
    daemon_security_add_acl_rule("external", "web_search", true);

    /* shell_run 默认放行：与 fs_* 一致，修复「LLM 一调用 shell_run 即被 ACL 拒绝」。
     * 仅当显式设置 AIRY_GATEWAY_ACL_ALLOW_SHELL=false/0 时才拒绝。 */
    const char *shell = getenv("AIRY_GATEWAY_ACL_ALLOW_SHELL");
    bool shell_allowed = !(shell && (strcmp(shell, "false") == 0 || strcmp(shell, "0") == 0));
    daemon_security_add_acl_rule("external", "shell_run", shell_allowed);

    SVC_LOG_INFO("Phase 3: Gateway ACL defaults registered "
                 "(external: fs_read/fs_write/fs_list ALLOW, shell_run=%s)",
                 shell_allowed ? "ALLOW" : "DENY");
}

/* ==================== 信号处理 ==================== */

/**
 * @brief L2 标准方法 <ns>.shutdown 回调（02-l2-service-protocol.md §6.1）
 *
 * 与信号处理路径一致：原子置位 g_running，主循环 1s 轮询内优雅退出。
 * 由 gateway_business_handle 收到 "shutdown" RPC 时经回调触发。
 */
static void gw_rpc_shutdown(void *user_data)
{
    (void)user_data;
    atomic_store_explicit(&g_running, 0, memory_order_seq_cst);
}

/**
 * @brief 信号处理函数（async-signal-safe：仅原子置位，真实停止动作由主循环完成）
 *
 * 禁止在信号处理器中调用锁/分配/日志（airy_mtx_lock、gateway_service_stop 等
 * 均非 async-signal-safe），否则主循环持锁时收到信号会死锁。
 */
static void signal_handler(int sig __attribute__((unused)))
{
    atomic_store_explicit(&g_running, 0, memory_order_seq_cst);
#ifndef _WIN32
    {
        static const char sig_msg[] =
            "[SIG] shutdown signal received, initiating graceful shutdown\n";
        (void)write(STDERR_FILENO, sig_msg, sizeof(sig_msg) - 1);
    }
#endif
}

#ifdef _WIN32
/**
 * @brief Windows控制台事件处理函数
 */
static BOOL WINAPI console_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        signal_handler((int)fdwCtrlType);
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static void svc_log_toggle_handler(int sig)
{
    (void)sig;
    static int debug_mode = 0;
    debug_mode = !debug_mode;
    log_set_module_level("*", debug_mode ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
}

/* ==================== 帮助信息 ==================== */

static void print_usage(const char *prog)
{
    char buf[256];
    fputs("AgentRT Gateway Daemon\n", stdout);
    snprintf(buf, sizeof(buf), "Usage: %s [options]\n\n", prog);
    fputs(buf, stdout);
    fputs("Options:\n", stdout);
    fputs("  -c <config>   Configuration file path\n", stdout);
    fputs("  -h <host>     HTTP gateway host (default: 0.0.0.0)\n", stdout);
    fputs("  -p <port>     HTTP gateway port (default: 8080)\n", stdout);
    fputs("  -w <port>     WebSocket gateway port (default: 8081)\n", stdout);
    fputs("  -s            Enable stdio gateway\n", stdout);
    fputs("  -d            Run as daemon (Unix only)\n", stdout);
    fputs("  -v            Verbose output\n", stdout);
    fputs("  --help        Show this help\n", stdout);
    fputs("\nExamples:\n", stdout);
    snprintf(buf, sizeof(buf), "  %s -h 127.0.0.1 -p 8080\n", prog);
    fputs(buf, stdout);
    snprintf(buf, sizeof(buf), "  %s -c AIRY_CONFIG_DIR \"/gateway.conf\"\n", prog);
    fputs(buf, stdout);
}

/* ==================== 参数解析 ==================== */

static int parse_args(int argc, char *argv[], gateway_service_config_t *config)
{
    gateway_service_get_default_config(config);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            airy_err_t err = gateway_service_load_config(config, argv[++i]);
            if (err != AIRY_SUCCESS) {
                SVC_LOG_ERROR("Failed to load config: %s", argv[i]);
                AIRY_ERROR(AIRY_ERR_IO, "failed to load config file");
            }
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            config->http.host = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            config->http.port = (uint16_t)strtol(argv[++i], NULL, 10);
            config->http.enabled = true;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            config->ws.port = (uint16_t)strtol(argv[++i], NULL, 10);
            config->ws.enabled = true;
        } else if (strcmp(argv[i], "-s") == 0) {
            config->stdio.enabled = true;
        } else if (strcmp(argv[i], "-v") == 0) {
            config->enable_metrics = true;
        } else if (strcmp(argv[i], "-d") == 0) {
#ifndef _WIN32
            pid_t pid = fork();
            if (pid < 0) {
                SVC_LOG_ERROR("Failed to fork");
                AIRY_ERROR(AIRY_ERR_UNKNOWN, "fork failed when daemonizing");
            }
            if (pid > 0)
                exit(0);
            setsid();
            umask(022);
            {
                int __rc __attribute__((unused)) = chdir("/");
            }
            fclose(stdin);
            fclose(stdout);
            fclose(stderr);
            SVC_LOG_INFO("Gateway daemonized");
#else
            SVC_LOG_WARN("-d not supported on Windows");
#endif
        } else {
            SVC_LOG_ERROR("Unknown option: %s", argv[i]);
            AIRY_ERROR(AIRY_ERR_INVALID_PARAM, "unknown option");
        }
    }
    return 0;
}

/* ==================== 主函数 ==================== */

int main(int argc, char *argv[])
{
    gateway_service_config_t config;

    /* E-3 资源确定性: 初始化与清理成对 */
    airy_sock_init();

    /* 跨平台信号处理 */
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, svc_log_toggle_handler);
#endif

    airy_log_init(NULL);
    atexit(log_cleanup);

    /* P3.14 ACC-DT15: 初始化 cupolas 安全穹顶（permission_engine + sanitizer + audit_logger）*/
    daemon_cupolas_init("gateway_d");

    if (parse_args(argc, argv, &config) != 0) {
        airy_sock_cleanup();
        return EXIT_FAILURE;
    }

    SVC_LOG_INFO("Gateway service starting...");

    airy_err_t err = gateway_service_create(&g_service, &config);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to create service (err=%d)", err);
        goto cleanup;
    }

    err = gateway_service_init(g_service);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to init service (err=%d)", err);
        goto cleanup_service;
    }

    /* 注册业务处理器：agent.run → llm_d 转发（修复 handler 未接线缺口） */
    g_biz_ctx = gateway_business_ctx_create();
    if (!g_biz_ctx) {
        SVC_LOG_ERROR("Failed to create business handler context");
        goto cleanup_service;
    }

    /* L2 标准方法 <ns>.shutdown（02-l2-service-protocol.md §6.1）：
     * 注入回调，收到 "shutdown" RPC 后触发与信号处理一致的优雅退出
     * （原子置位 g_running，主循环 1s 轮询内退出）。 */
    gateway_business_ctx_set_shutdown_cb(g_biz_ctx, gw_rpc_shutdown, NULL);

    /* Phase 3 R4: 协议层 ACL 默认规则（daemon_security 已由 cupolas 初始化） */
    gw_acl_register_defaults();

    /* Phase 2: 协议路由器（MCP/OpenAI/A2A 适配器注册于此） */
    g_proto_router = gw_proto_router_create();
    if (!g_proto_router) {
        SVC_LOG_ERROR("Failed to create protocol router");
        gateway_business_ctx_destroy(g_biz_ctx);
        g_biz_ctx = NULL;
        goto cleanup_service;
    }
    if (gw_proto_router_init(g_proto_router) != 0) {
        SVC_LOG_ERROR("Failed to init protocol router");
        gw_proto_router_destroy(g_proto_router);
        g_proto_router = NULL;
        gateway_business_ctx_destroy(g_biz_ctx);
        g_biz_ctx = NULL;
        goto cleanup_service;
    }

    /* Phase 2: 适配器接线 —— MCP 工具 → tool_d / OpenAI → llm_d / A2A → sched_d
     * （协议翻译集中在网关，daemon 侧零协议知识，D2） */
    {
        /* MCP: 注册内置工具，exec_fn 直连 tool_d.execute_tool */
        gw_mcp_server_t *mcp = gw_proto_router_get_mcp(g_proto_router);
        if (mcp) {
            gw_mcp_server_register_tool(mcp, "fs_read",
                                        "Read a file's content from the local filesystem",
                                        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
                                        gw_biz_tool_exec, g_biz_ctx);
            gw_mcp_server_register_tool(mcp, "fs_write",
                                        "Write content to a local file (creates or overwrites)",
                                        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
                                        gw_biz_tool_exec, g_biz_ctx);
            gw_mcp_server_register_tool(mcp, "fs_list",
                                        "List entries of a local directory (JSON array)",
                                        /* 与 tool_d 注册一致：path 必填（tool_d validator
                                         * 对注册参数一律校验存在性，schema 声明可选会导致
                                         * LLM/MCP 不传 path 而 tool_d 校验失败） */
                                        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
                                        gw_biz_tool_exec, g_biz_ctx);
            gw_mcp_server_register_tool(mcp, "shell_run",
                                        "Execute a shell command and capture its output",
                                        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}",
                                        gw_biz_tool_exec, g_biz_ctx);
            gw_mcp_server_register_tool(mcp, "web_fetch",
                                        "Fetch a web page over HTTP(S) and return its body text",
                                        "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}",
                                        gw_biz_tool_exec, g_biz_ctx);
            /* 与 tool_d 内置工具保持一致（tools[9]），补齐剩余 4 个工具 */
            gw_mcp_server_register_tool(mcp, "fs_glob",
                                        "List files matching a glob pattern (supports * ? and **)",
                                        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"base\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}",
                                        gw_biz_tool_exec, g_biz_ctx);
            gw_mcp_server_register_tool(mcp, "fs_grep",
                                        "Search file contents with a regular expression (relpath:line:text)",
                                        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"glob\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},\"required\":[\"pattern\"]}",
                                        gw_biz_tool_exec, g_biz_ctx);
            gw_mcp_server_register_tool(mcp, "fs_edit",
                                        "Replace an exact string in a file (search-and-replace edit)",
                                        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old\":{\"type\":\"string\"},\"new\":{\"type\":\"string\"},\"count\":{\"type\":\"integer\"}},\"required\":[\"path\",\"old\",\"new\"]}",
                                        gw_biz_tool_exec, g_biz_ctx);
            gw_mcp_server_register_tool(mcp, "web_search",
                                        "Search the web (DuckDuckGo) and return ranked results",
                                        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},\"required\":[\"query\"]}",
                                        gw_biz_tool_exec, g_biz_ctx);
            SVC_LOG_INFO("Phase 2: MCP adapter wired — 9 tools → tool_d");
            /* P2-4: 消费外部 MCP server 工具（AIRY_MCP_CLIENTS），失败仅告警 */
            gw_mcp_clients_setup(mcp);
        }

        /* OpenAI: chat/completions → llm_d.complete */
        gw_openai_compat_t *openai = gw_proto_router_get_openai(g_proto_router);
        if (openai) {
            gw_openai_compat_set_llm_call(openai, gw_biz_llm_complete, g_biz_ctx);
            SVC_LOG_INFO("Phase 2: OpenAI adapter wired — chat/completions → llm_d");
        }

        /* A2A: task → sched_d.schedule_task（注册常用任务类型） */
        gw_a2a_handler_t *a2a = gw_proto_router_get_a2a(g_proto_router);
        if (a2a) {
            static const char *a2a_task_types[] = {
                "coding", "analysis", "summarize", "general", "devops", NULL};
            for (int i = 0; a2a_task_types[i]; i++) {
                gw_a2a_handler_register_task_type(a2a, a2a_task_types[i],
                                                  gw_biz_sched_schedule, g_biz_ctx);
            }
            SVC_LOG_INFO("Phase 2: A2A adapter wired — task → sched_d");
        }
    }

    /* 统一协议入口：协议检测路由（MCP/OpenAI/A2A）+ JSON-RPC 业务（agent.run/ping） */
    g_entry_ctx.biz_ctx = g_biz_ctx;
    g_entry_ctx.router = g_proto_router;
    err = gateway_service_set_handler(g_service, gateway_protocol_entry, &g_entry_ctx);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to register protocol entry handler (err=%d)", err);
        gw_proto_router_destroy(g_proto_router);
        g_proto_router = NULL;
        gateway_business_ctx_destroy(g_biz_ctx);
        g_biz_ctx = NULL;
        goto cleanup_service;
    }
    SVC_LOG_INFO("Protocol entry handler registered (MCP/OpenAI/A2A/JSON-RPC)");

    /* Initialize UnifiedProtocol stack for multi-protocol support */
#ifdef AIRY_HAS_PROTOCOLS
    const protocol_adapter_t *mcp_adapter = mcp_v1_get_adapter();
    if (mcp_adapter) {
        if (mcp_adapter->init(mcp_adapter->context) == 0) {
            SVC_LOG_INFO("MCP v1.0 adapter initialized (version=%s, caps=0x%x)",
                         mcp_adapter->version ? mcp_adapter->version : "unknown",
                         mcp_adapter->capabilities ? mcp_adapter->capabilities(mcp_adapter->context)
                                                   : 0);
        } else {
            SVC_LOG_WARN("Failed to initialize MCP v1.0 adapter");
        }
    } else {
        SVC_LOG_WARN("MCP v1.0 adapter not available");
    }
#endif

    err = gateway_service_start(g_service);
    if (err != AIRY_SUCCESS) {
        SVC_LOG_ERROR("Failed to start service (err=%d)", err);
        goto cleanup_service;
    }

    SVC_LOG_INFO("AgentRT Gateway Daemon started");
    SVC_LOG_INFO("  HTTP:     %s:%d %s", config.http.host, config.http.port,
                 config.http.enabled ? "[enabled]" : "[disabled]");
    SVC_LOG_INFO("  WebSocket: %s:%d %s", config.ws.host, config.ws.port,
                 config.ws.enabled ? "[enabled]" : "[disabled]");
    SVC_LOG_INFO("  Stdio:    %s", config.stdio.enabled ? "[enabled]" : "[disabled]");

    g_bsd = daemon_bootstrap_sd_start("gateway_d", "gateway", config.http.host,
                                      config.http.port, "gateway,core", 0);
    g_bipc = daemon_bootstrap_ipc_start("gateway_d", "gateway", config.http.host,
                                        config.http.port, IPC_BUS_PROTO_JSON_RPC);

    /* 主事件循环：信号驱动 + 周期性健康检查 */
    int loop_count = 0;
    const int HEALTH_CHECK_INTERVAL = 30;

    while (atomic_load_explicit(&g_running, memory_order_acquire)) {
        if (!gateway_service_is_running(g_service)) {
            SVC_LOG_WARN("Gateway service stopped unexpectedly");
            break;
        }

        airy_sleep_ms(1000);
        loop_count++;

        if (config.enable_metrics && (loop_count % HEALTH_CHECK_INTERVAL == 0)) {
            airy_svc_stats_t stats;
            if (gateway_service_get_stats(g_service, &stats) == AIRY_SUCCESS) {
                SVC_LOG_INFO("Health Check [interval=%ds] "
                             "| concurrent=%u | total_req=%llu "
                             "| errors=%llu | avg_time=%.1fms",
                             HEALTH_CHECK_INTERVAL, stats.current_concurrent,
                             (unsigned long long)stats.request_count,
                             (unsigned long long)stats.error_count, stats.avg_time_ms);
            } else {
                SVC_LOG_WARN("Health check failed: unable to retrieve service stats");
            }
        }
    }

    daemon_bootstrap_ipc_stop(g_bipc);
    daemon_bootstrap_sd_stop(g_bsd);

    SVC_LOG_INFO("Gateway shutting down...");
    gateway_service_stop(g_service, false);

    /* Cleanup protocol stack */
#ifdef AIRY_HAS_PROTOCOLS
    {
        const protocol_adapter_t *mcp_adapter = mcp_v1_get_adapter();
        if (mcp_adapter && mcp_adapter->destroy) {
            mcp_adapter->destroy(mcp_adapter->context);
            SVC_LOG_INFO("MCP adapter destroyed");
        }
    }
#endif

cleanup_service:
    /* P2-4: 先断开外部 MCP server，再销毁本地 MCP 工具表 */
    gw_mcp_client_cleanup();
    if (g_proto_router) {
        gw_proto_router_destroy(g_proto_router);
        g_proto_router = NULL;
    }
    if (g_biz_ctx) {
        gateway_business_ctx_destroy(g_biz_ctx);
        g_biz_ctx = NULL;
    }
    gateway_service_destroy(g_service);
cleanup:
    airy_sock_cleanup();

    SVC_LOG_INFO("Gateway daemon stopped");
    daemon_cupolas_cleanup(); /* P3.14 ACC-DT15: 清理 cupolas 安全穹顶 */
    log_cleanup();
    return 0;
}
