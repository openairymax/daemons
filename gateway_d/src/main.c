// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/*
 * @file main.c
 * @brief Gateway daemon main entry (daemon module conventions).
 *
 * Conventions followed:
 * - ARCHITECTURAL_PRINCIPLES.md E-3 resource determinism (paired management)
 * - ARCHITECTURAL_PRINCIPLES.md E-4 cross-platform consistency (platform.h)
 * - ARCHITECTURAL_PRINCIPLES.md E-5 semantic naming (SVC_LOG_*)
 * - ARCHITECTURAL_PRINCIPLES.md E-6 traceable errors (AIRY_ERR_*)
 */

#include "atomic_compat.h"
#include "daemon_bootstrap_sd.h"
#include "daemon_bootstrap_ipc.h"
#include "daemon_cupolas_bootstrap.h"

#include "daemon_heapstore_bootstrap.h"
#include "gateway_service.h"
#include "gateway_business_handler.h"
#include "gateway_biz_internal.h"
#include "daemon_security.h"
#include "logging.h"
#include "daemon_platform_ext.h"
#include "svc_common.h"
#include "svc_config.h"
#include "svc_logger.h"
#include "error.h"
#include "airy_memory.h"

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

#include "mcp_client.h"
#include <cjson/cJSON.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h> /* umask */
#include <unistd.h>
#endif

static gateway_service_t g_service = NULL;
static atomic_int g_running = 1;
static daemon_bootstrap_sd_t *g_bsd = NULL;
static daemon_bootstrap_ipc_t *g_bipc = NULL;
static gateway_business_ctx_t *g_biz_ctx = NULL;
static gateway_entry_ctx_t g_entry_ctx;
static gw_proto_router_t *g_proto_router = NULL;

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
static void gw_mcp_client_cleanup(void)
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

/**
 * @brief Register default ACL rules for external protocols (fail-closed
 *        requires explicit authorization)
 *
 * External protocol requests use the unified identity "external" (see
 * gateway_business_handler.c):
 *   - fs_read/fs_write/fs_list basic tools allowed by default (do not block
 *     legitimate requests)
 *   - shell_run allowed by default (same as basic tools): gateway exposes
 *     shell_run as a tool schema to the LLM (GW_TOOLS_JSON); if denied by
 *     default, the LLM is ACL-blocked on first call, breaking the whole tool
 *     chain. To disable, set AIRY_GATEWAY_ACL_ALLOW_SHELL=false to deny
 *     explicitly (only "false"/"0" count as off; other values allow).
 *
 * Must run after daemon_security initialization (daemon_cupolas_init).
 */
static void gw_acl_register_defaults(void)
{
    daemon_security_add_acl_rule("external", "fs_read", true);
    daemon_security_add_acl_rule("external", "fs_write", true);
    daemon_security_add_acl_rule("external", "fs_list", true);

    daemon_security_add_acl_rule("external", "web_fetch", true);
    /* fs_glob/fs_grep read-only search, fs_edit controlled replacement,
     * web_search read-only web search — same trust level as the fs_* basic
     * tools, allowed by default (otherwise the LLM is ACL-blocked as soon as
     * it calls them via MCP) */
    daemon_security_add_acl_rule("external", "fs_glob", true);
    daemon_security_add_acl_rule("external", "fs_grep", true);
    daemon_security_add_acl_rule("external", "fs_edit", true);
    daemon_security_add_acl_rule("external", "web_search", true);

    /* shell_run allowed by default: same as fs_*; fixes "the LLM is
     * ACL-rejected as soon as it calls shell_run". Only explicitly setting
     * AIRY_GATEWAY_ACL_ALLOW_SHELL=false/0 denies it. */
    const char *shell = getenv("AIRY_GATEWAY_ACL_ALLOW_SHELL");
    bool shell_allowed = !(shell && (strcmp(shell, "false") == 0 || strcmp(shell, "0") == 0));
    daemon_security_add_acl_rule("external", "shell_run", shell_allowed);

    SVC_LOG_INFO("Phase 3: Gateway ACL defaults registered "
                 "(external: fs_read/fs_write/fs_list ALLOW, shell_run=%s)",
                 shell_allowed ? "ALLOW" : "DENY");
}

/**
 * @brief L2 standard method <ns>.shutdown callback (02-l2-service-protocol.md
 *        §6.1)
 *
 * Consistent with the signal-handling path: atomically clear g_running; the
 * main loop exits gracefully within its 1s poll. Triggered via the callback
 * when gateway_business_handle receives a "shutdown" RPC.
 */
static void gw_rpc_shutdown(void *user_data)
{
    (void)user_data;
    atomic_store_explicit(&g_running, 0, memory_order_seq_cst);
}

/**
 * @brief Signal handler (async-signal-safe: only sets an atomic flag; the
 *        actual stop action is done by the main loop)
 *
 * Must not call lock/alloc/log inside a signal handler (airy_mtx_lock,
 * gateway_service_stop etc. are not async-signal-safe); otherwise a signal
 * received while the main loop holds a lock would deadlock.
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
 * @brief Windows console event handler
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

static int parse_args(int argc, char *argv[], gateway_service_config_t *config)
{
    gateway_service_get_default_config(config);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--manager") == 0 && i + 1 < argc) {
            /* 兼容 bootstrap 统一 daemon 启动参数：--manager 指向 agentrt.yaml
             * 全局配置（bootstrap 对全部 daemon 一致传参）。gateway_d 细粒度
             * 配置仍以默认值 + 环境变量为准（load_config 仅解析 key=value，
             * 不解析 YAML；缺失时保持默认 8080/8081，与 agentrt.yaml 一致）。 */
            i++;
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

int main(int argc, char *argv[])
{
    gateway_service_config_t config;

    airy_sock_init();

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

    daemon_cupolas_init("gateway_d");

    daemon_heapstore_init("gateway_d");

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

    g_biz_ctx = gateway_business_ctx_create();
    if (!g_biz_ctx) {
        SVC_LOG_ERROR("Failed to create business handler context");
        goto cleanup_service;
    }

    /* L2 standard method <ns>.shutdown (02-l2-service-protocol.md §6.1):
     * inject the callback so that after receiving a "shutdown" RPC, graceful
     * exit identical to signal handling is triggered (atomically clear
     * g_running; the main loop exits within its 1s poll). */
    gateway_business_ctx_set_shutdown_cb(g_biz_ctx, gw_rpc_shutdown, NULL);

    gw_acl_register_defaults();

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

    /* Phase 2: adapter wiring — MCP tools -> tool_d / OpenAI -> llm_d /
     * A2A -> sched_d (protocol translation is concentrated in the gateway;
     * daemons have zero protocol knowledge, D2) */
    {

        gw_mcp_server_t *mcp = gw_proto_router_get_mcp(g_proto_router);
        if (mcp) {
            /* 内置工具 schema 唯一权威在 gateway_biz_tools.c（S-6 SSoT），
             * 与 tool_d service_builtin.c 参数定义一致；一致性由
             * tests/test_mcp_tools_schema.c 门禁。 */
            int t_failed = gw_biz_mcp_register_tools(mcp, g_biz_ctx);
            if (t_failed != 0) {
                SVC_LOG_WARN("Phase 2: %d builtin tool(s) failed to register", t_failed);
            }
            SVC_LOG_INFO("Phase 2: MCP adapter wired — 9 tools → tool_d");

            gw_mcp_clients_setup(mcp);
        }

        /* OpenAI: chat/completions → llm_d.complete */
        gw_openai_compat_t *openai = gw_proto_router_get_openai(g_proto_router);
        if (openai) {
            gw_openai_compat_set_llm_call(openai, gw_biz_llm_complete, g_biz_ctx);
            /* OpenAI: embeddings → llm_d.embeddings（RAG/知识库生态接入点） */
            gw_openai_compat_set_embed_fn(openai, gw_biz_llm_embeddings, g_biz_ctx);
            SVC_LOG_INFO("Phase 2: OpenAI adapter wired — chat/completions + embeddings → llm_d");
        }

        gw_a2a_handler_t *a2a = gw_proto_router_get_a2a(g_proto_router);
        if (a2a) {
            static const char *a2a_task_types[] = {"coding",  "analysis", "summarize",
                                                   "general", "devops",   NULL};
            for (int i = 0; a2a_task_types[i]; i++) {
                gw_a2a_handler_register_task_type(a2a, a2a_task_types[i], gw_biz_sched_schedule,
                                                  g_biz_ctx);
            }
            SVC_LOG_INFO("Phase 2: A2A adapter wired — task → sched_d");
        }
    }

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
                         mcp_adapter->capabilities ?
                             mcp_adapter->capabilities(mcp_adapter->context) :
                             0);
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

    g_bsd = daemon_bootstrap_sd_start("gateway_d", "gateway", config.http.host, config.http.port,
                                      "gateway,core", 0);
    g_bipc = daemon_bootstrap_ipc_start("gateway_d", "gateway", config.http.host, config.http.port,
                                        IPC_BUS_PROTO_JSON_RPC);

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
    daemon_heapstore_cleanup();
    daemon_cupolas_cleanup();
    log_cleanup();
    return 0;
}
