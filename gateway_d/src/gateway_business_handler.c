// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_business_handler.c
 * @brief 网关业务请求处理器：ctx 生命周期 + 主分发 + 协议入口
 *
 * SEC-017 合规：所有功能均为真实实现，无桩函数。
 * 处理链：HTTP JSON-RPC agent.run → 双思考/编排 → 返回对话结果。
 *
 * 本文件按单一职责拆分自原 2608 行单体（2026-08-11）：
 *   - gateway_biz_forward.c  命名空间转发（L2 协议客户端 + 白名单）
 *   - gateway_biz_llm.c      LLM 调用 + 工具循环（ReAct）
 *   - gateway_biz_agent.c    agent.run 编排（双思考注入 + 取消）
 *   - gateway_biz_backend.c  MCP/OpenAI/A2A 协议后端
 * 本文件保留：ctx 生命周期、JSON-RPC 主分发、协议检测入口。
 */

#include "gateway_business_handler.h"

#include "gateway_biz_internal.h"

#include "logging.h"
#include "platform.h"
#include "gateway_protocol_router.h"
#include "daemon_security.h"

#include "svc_model_defaults.h"

#include "daemon_heapstore_bootstrap.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Resolve the daemon Unix socket path: <ENV_NAME> override ->
 * airy_runtime_dir()/<sock_name> -> <sock_name>. Kept consistent with the
 * daemon-side single source of truth: airy_runtime_dir() resolves $AIRY_HOME/run,
 * defaulting to ~/.airymaxrt/run */
static void gw_resolve_daemon_sock(char *out, size_t out_size, const char *env_name,
                                   const char *sock_name)
{
    const char *env = getenv(env_name);
    if (env && *env) {
        AIRY_STRNCPY_TERM(out, env, out_size);
        return;
    }
    const char *run_dir = airy_runtime_dir();
    if (run_dir && *run_dir) {
        snprintf(out, out_size, "%s/%s", run_dir, sock_name);
    } else {
        AIRY_STRNCPY_TERM(out, sock_name, out_size);
    }
}

gateway_business_ctx_t *gateway_business_ctx_create(void)
{
    gateway_business_ctx_t *ctx =
        (gateway_business_ctx_t *)AIRY_CALLOC(1, sizeof(gateway_business_ctx_t));
    if (!ctx)
        return NULL;

    airy_mtx_init(&ctx->active_lock);
    ctx->active_requests = NULL;

    /* Each daemon socket: <DAEMON>_SOCK env -> $AIRY_RUNTIME_DIR/<name>.sock ->
     * $AIRY_HOME/run/<name>.sock */
    gw_resolve_daemon_sock(ctx->llm_sock_path, sizeof(ctx->llm_sock_path), "AIRY_LLM_SOCK",
                           "llm.sock");
    gw_resolve_daemon_sock(ctx->tool_sock_path, sizeof(ctx->tool_sock_path), "AIRY_TOOL_SOCK",
                           "tool.sock");
    gw_resolve_daemon_sock(ctx->agent_sock_path, sizeof(ctx->agent_sock_path), "AIRY_AGENT_SOCK",
                           "agent.sock");
    gw_resolve_daemon_sock(ctx->mem_sock_path, sizeof(ctx->mem_sock_path), "AIRY_MEM_SOCK",
                           "mem.sock");
    gw_resolve_daemon_sock(ctx->sched_sock_path, sizeof(ctx->sched_sock_path), "AIRY_SCHED_SOCK",
                           "sched.sock");
    gw_resolve_daemon_sock(ctx->think_sock_path, sizeof(ctx->think_sock_path), "AIRY_THINK_SOCK",
                           "think.sock");
    gw_resolve_daemon_sock(ctx->a2a_sock_path, sizeof(ctx->a2a_sock_path), "AIRY_A2A_SOCK",
                           "a2a.sock");
    gw_resolve_daemon_sock(ctx->plugin_sock_path, sizeof(ctx->plugin_sock_path), "AIRY_PLUGIN_SOCK",
                           "plugin.sock");
    gw_resolve_daemon_sock(ctx->info_sock_path, sizeof(ctx->info_sock_path), "AIRY_INFO_SOCK",
                           "info.sock");
    gw_resolve_daemon_sock(ctx->notify_sock_path, sizeof(ctx->notify_sock_path), "AIRY_NOTIFY_SOCK",
                           "notify.sock");
    gw_resolve_daemon_sock(ctx->observe_sock_path, sizeof(ctx->observe_sock_path),
                           "AIRY_OBSERVE_SOCK", "observe.sock");
    gw_resolve_daemon_sock(ctx->market_sock_path, sizeof(ctx->market_sock_path), "AIRY_MARKET_SOCK",
                           "market.sock");
    gw_resolve_daemon_sock(ctx->hook_sock_path, sizeof(ctx->hook_sock_path), "AIRY_HOOK_SOCK",
                           "hook.sock");
    gw_resolve_daemon_sock(ctx->monit_sock_path, sizeof(ctx->monit_sock_path), "AIRY_MONIT_SOCK",
                           "monit.sock");
    gw_resolve_daemon_sock(ctx->channel_sock_path, sizeof(ctx->channel_sock_path),
                           "AIRY_CHANNEL_SOCK", "channel.sock");
    gw_resolve_daemon_sock(ctx->cupolas_sock_path, sizeof(ctx->cupolas_sock_path),
                           "AIRY_CUPOLAS_SOCK", "cupolas.sock");

    const char *tcp_env = getenv("AIRY_LLM_TCP_ADDR");
    AIRY_STRNCPY_TERM(ctx->llm_tcp_addr, (tcp_env && *tcp_env) ? tcp_env : "127.0.0.1",
                      sizeof(ctx->llm_tcp_addr));
    const char *port_env = getenv("AIRY_LLM_TCP_PORT");
    ctx->llm_tcp_port =
        (port_env && *port_env) ? (uint16_t)atoi(port_env) : GW_LLM_DEFAULT_TCP_PORT;

    /* Default model: env AIRY_AGENT_MODEL > user override
     * $AIRY_CONFIG_DIR/model.yaml global.default_model > built-in default
     * (aligned with model.yaml). Users need not touch the repo SSoT; overriding
     * the global section in $AIRY_HOME/config/model.yaml applies to both
     * gateway and llm_d (same resolution path). */
    const char *model_env = getenv("AIRY_AGENT_MODEL");
    if (model_env && *model_env) {
        AIRY_STRNCPY_TERM(ctx->default_model, model_env, sizeof(ctx->default_model));
    } else {
        char um[128] = {0};
        const char *cfg_dir = airy_config_dir();
        int has_user_cfg = 0;
        if (cfg_dir) {
            char user_path[1024];
            int plen = snprintf(user_path, sizeof(user_path), "%s/model.yaml", cfg_dir);
            if (plen > 0 && plen < (int)sizeof(user_path)) {
                FILE *uf = fopen(user_path, "rb");
                if (uf) {
                    fclose(uf);
                    if (svc_model_defaults_from_yaml(user_path, um, sizeof(um), NULL, 0) == 0 &&
                        um[0])
                        has_user_cfg = 1;
                    else {
                        /* No global section: fall back to the simple llm
                         * section model (same semantics as llm_d; the default
                         * model configured under llm: also applies to gateway) */
                        svc_model_llm_config_t llm_cfg;
                        AIRY_MEMSET(&llm_cfg, 0, sizeof(llm_cfg));
                        if (svc_model_defaults_llm_from_yaml(user_path, &llm_cfg) == 0 &&
                            llm_cfg.model[0]) {
                            AIRY_STRNCPY_TERM(um, llm_cfg.model, sizeof(um));
                            has_user_cfg = 1;
                        }
                    }
                }
            }
        }
        AIRY_STRNCPY_TERM(ctx->default_model, has_user_cfg ? um : GW_LLM_DEFAULT_MODEL,
                          sizeof(ctx->default_model));
    }

    return ctx;
}

int gateway_business_ctx_set_shutdown_cb(gateway_business_ctx_t *ctx, gateway_shutdown_fn_t cb,
                                         void *user_data)
{
    if (!ctx)
        return AIRY_ERR_INVALID_PARAM;
    ctx->on_shutdown = cb;
    ctx->shutdown_user_data = user_data;
    return AIRY_SUCCESS;
}

void gateway_business_ctx_destroy(gateway_business_ctx_t *ctx)
{
    if (!ctx)
        return;

    airy_mtx_lock(&ctx->active_lock);
    gw_active_request_t *entry = ctx->active_requests;
    ctx->active_requests = NULL;
    airy_mtx_unlock(&ctx->active_lock);
    while (entry) {
        gw_active_request_t *next = entry->next;
        AIRY_FREE(entry);
        entry = next;
    }
    airy_mtx_destroy(&ctx->active_lock);
    AIRY_FREE(ctx);
}

char *gateway_business_handle(void *request, void *user_data)
{
    const char *req = (const char *)request;
    gateway_business_ctx_t *ctx = (gateway_business_ctx_t *)user_data;
    if (!req || !ctx) {
        return jsonrpc_error(-32600, "Invalid request", NULL);
    }

    cJSON *root = cJSON_Parse(req);
    if (!root) {
        return jsonrpc_error(-32700, "Parse error", NULL);
    }

    cJSON *method = cJSON_GetObjectItem(root, "method");
    if (!cJSON_IsString(method)) {
        cJSON_Delete(root);
        return jsonrpc_error(-32600, "Invalid Request", NULL);
    }

    daemon_heapstore_log("gateway_d", 1, method->valuestring, NULL);

    char *resp = NULL;
    if (strcmp(method->valuestring, "agent.run") == 0) {
        resp = handle_agent_run(root, ctx);
    } else if (strcmp(method->valuestring, "agent.cancel") == 0) {
        resp = handle_agent_cancel(root, ctx);
    } else if (strcmp(method->valuestring, "llm.list_models") == 0) {
        resp = handle_llm_list_models(root, ctx);
    } else if (strncmp(method->valuestring, "llm.", 4) == 0) {
        /* llm.list_models is handled by the dedicated branch above (adds
         * default_model/default_provider); all other llm.* methods
         * (complete/count_tokens/health_check/get_stats) forward generically */
        gw_ns_forward_rule_t r2 = GW_NS_LLM;
        r2.sock_path = ctx->llm_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "mem.", 4) == 0) {
        resp = handle_mem_call(root, ctx);
    } else if (strcmp(method->valuestring, "tool.pending") == 0) {
        resp = handle_tool_approval_call(root, ctx, "pending");
    } else if (strcmp(method->valuestring, "tool.approve") == 0) {
        resp = handle_tool_approval_call(root, ctx, "approve");
    } else if (strncmp(method->valuestring, "agent.", 6) == 0) {
        /* agent.run / agent.cancel use the dedicated orchestration branches
         * above; other agent.* methods (spawn/list/count/health_check/get_stats)
         * forward generically */
        gw_ns_forward_rule_t r2 = GW_NS_AGENT;
        r2.sock_path = ctx->agent_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "tool.", 5) == 0) {
        /* tool.pending / tool.approve use the dedicated branches above
         * (approval flow); other tool.* methods (list/execute/health_check)
         * forward generically */
        gw_ns_forward_rule_t r2 = GW_NS_TOOL;
        r2.sock_path = ctx->tool_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "a2a.", 4) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_A2A;
        r2.sock_path = ctx->a2a_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "plugin.", 7) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_PLUGIN;
        r2.sock_path = ctx->plugin_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "info.", 5) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_INFO;
        r2.sock_path = ctx->info_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "notify.", 7) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_NOTIFY;
        r2.sock_path = ctx->notify_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "observe.", 8) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_OBSERVE;
        r2.sock_path = ctx->observe_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "market.", 7) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_MARKET;
        r2.sock_path = ctx->market_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "hook.", 5) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_HOOK;
        r2.sock_path = ctx->hook_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "sched.", 6) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_SCHED;
        r2.sock_path = ctx->sched_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "think.", 6) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_THINK;
        r2.sock_path = ctx->think_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "monit.", 6) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_MONIT;
        r2.sock_path = ctx->monit_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "channel.", 8) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_CHANNEL;
        r2.sock_path = ctx->channel_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "cupolas.", 8) == 0) {
        gw_ns_forward_rule_t r2 = GW_NS_CUPOLAS;
        r2.sock_path = ctx->cupolas_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strcmp(method->valuestring, "ping") == 0) {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "jsonrpc", "2.0");
        if (id && cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(out, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(out, "id");
        }
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "ok");
        cJSON_AddItemToObject(out, "result", result);
        resp = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
    } else if (strcmp(method->valuestring, "shutdown") == 0) {
        /* Standard L2 method <ns>.shutdown (02-l2-service-protocol.md §6.1:
         * graceful stop). Build the success response first, then invoke the
         * host callback to trigger the real graceful exit (main loop exits),
         * so the response is not cut off by the shutdown. */
        cJSON *id = cJSON_GetObjectItem(root, "id");
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "jsonrpc", "2.0");
        if (id && cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(out, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(out, "id");
        }
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "shutting_down");
        cJSON_AddItemToObject(out, "result", result);
        resp = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
        if (ctx->on_shutdown) {
            ctx->on_shutdown(ctx->shutdown_user_data);
        } else {
            LOG_WARN("gateway: shutdown requested but no shutdown callback registered");
        }
    } else {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        resp = jsonrpc_error(-32601, "Method not found", id);
    }

    cJSON_Delete(root);
    return resp;
}

static int is_mcp_jsonrpc_method(const char *method)
{
    static const char *mcp_methods[] = {
        "initialize",
        "tools/list",
        "tools/call",
        "resources/list",
        "resources/read",
        "prompts/list",
        "notifications/initialized",
        NULL,
    };
    if (!method)
        return 0;
    for (int i = 0; mcp_methods[i]; i++) {
        if (strcmp(method, mcp_methods[i]) == 0)
            return 1;
    }

    return 0;
}

static int is_a2a_jsonrpc_method(const char *method)
{
    static const char *a2a_methods[] = {
        "tasks/send",   "tasks/get",      "tasks/cancel",       "tasks/pushNotification",
        "message/send", "agent-card/get", "agent/getAgentCard", NULL,
    };
    if (!method)
        return 0;
    for (int i = 0; a2a_methods[i]; i++) {
        if (strcmp(method, a2a_methods[i]) == 0)
            return 1;
    }
    /* Note: must not hijack by the "a2a." prefix — a2a.* is also a JSON-RPC
     * business namespace (gateway -> a2a_d forwarding chain, e.g.
     * a2a.discover_agents). The earlier prefix matching misrouted business
     * methods to the A2A protocol handler (proto=A2A) causing -32603. */
    return 0;
}

char *gateway_protocol_entry(void *request, void *user_data)
{
    const char *body = (const char *)request;
    const gateway_entry_ctx_t *ectx = (const gateway_entry_ctx_t *)user_data;
    if (!body || !ectx || !ectx->biz_ctx || !ectx->router) {
        return jsonrpc_error(-32600, "Invalid request", NULL);
    }

    gw_proto_detect_result_t proto = gw_proto_detect(NULL, NULL, body);

    if (proto == GW_PROTO_DETECT_JSONRPC) {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            cJSON *m = cJSON_GetObjectItem(root, "method");
            if (cJSON_IsString(m)) {
                if (is_mcp_jsonrpc_method(m->valuestring)) {
                    proto = GW_PROTO_DETECT_MCP;
                } else if (is_a2a_jsonrpc_method(m->valuestring)) {
                    proto = GW_PROTO_DETECT_A2A;
                }
            }
            cJSON_Delete(root);
        }
    }

    if (proto == GW_PROTO_DETECT_MCP || proto == GW_PROTO_DETECT_OPENAI ||
        proto == GW_PROTO_DETECT_A2A) {
        char *resp = NULL;
        int rc = gw_proto_router_route((gw_proto_router_t *)ectx->router, proto, "POST", NULL, body,
                                       &resp);
        if (rc != 0 || !resp) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Protocol handler failed: proto=%d rc=%d", (int)proto, rc);
            return jsonrpc_error(-32603, msg, NULL);
        }
        return resp;
    }

    return gateway_business_handle(request, ectx->biz_ctx);
}
