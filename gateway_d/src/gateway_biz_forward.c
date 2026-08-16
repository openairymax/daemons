// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_biz_forward.c
 * @brief Gateway namespace forwarding: L2 protocol client + method whitelist.
 *
 * Acts as the gateway -> daemon L2 service-protocol client
 * (<daemon>.<method>), providing a unified Unix-socket JSON-RPC call
 * (gw_svc_call), and maintains a method whitelist per external namespace;
 * methods not listed always return -32601 (preventing arbitrary method
 * passthrough).
 *
 * Split from gateway_business_handler.c (single responsibility: namespace
 * forwarding).
 */

#include "gateway_biz_internal.h"

#include "logging.h"
#include "platform.h"
#include "daemon_security.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

char *jsonrpc_error(int code, const char *msg, const cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp)
        return NULL;
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg ? msg : "Unknown error");
    cJSON_AddItemToObject(resp, "error", err);

    if (id && !cJSON_IsNull(id)) {
        if (cJSON_IsString(id)) {
            cJSON_AddStringToObject(resp, "id", id->valuestring);
        } else if (cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(resp, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(resp, "id");
        }
    } else {
        cJSON_AddNullToObject(resp, "id");
    }

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return out;
}

/**
 * @brief Generic daemon internal service call (Unix socket JSON-RPC)
 *
 * Builds {"jsonrpc":"2.0","method":<method>,"params":<params_json>,"id":1},
 * sends it to the target daemon socket and blocks until the full JSON response
 * is read. The gateway acts as a client of the L2 service protocol
 * (<daemon>.<method>) to call each daemon; daemons need no knowledge of the
 * external protocol.
 *
 * @param sock_path   Target daemon socket path
 * @param method      Internal service method (e.g. "spawn"/"invoke"/"write")
 * @param params_json Method params JSON string (NULL/empty -> "{}")
 * @param timeout_ms  Receive timeout (ms)
 * @return Response JSON string (AIRY_MALLOC, caller AIRY_FREE), or NULL on failure
 */
char *gw_svc_call(const char *sock_path, const char *method, const char *params_json,
                  int timeout_ms)
{
#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }

    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        close(fd);
        return NULL;
    }
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", method);
    if (params_json && params_json[0]) {
        cJSON *p = cJSON_Parse(params_json);
        cJSON_AddItemToObject(req, "params", p ? p : cJSON_CreateObject());
    } else {
        cJSON_AddItemToObject(req, "params", cJSON_CreateObject());
    }
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) {
        close(fd);
        return NULL;
    }

    size_t len = strlen(req_str);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_str + sent, len - sent, 0);
        if (n <= 0) {
            AIRY_FREE(req_str);
            close(fd);
            return NULL;
        }
        sent += (size_t)n;
    }
    AIRY_FREE(req_str);

    size_t cap = 65536;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
        close(fd);
        return NULL;
    }
    resp[0] = '\0';
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > GW_LLM_MAX_RESP) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
                close(fd);
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }
    close(fd);
    return resp;
#else
    /* Windows：daemon 统一走 TCP 回环（daemon_main.h parse_args 强制），
     * sock_path 参数约定为 "host:port"（如 "127.0.0.1:8086"），与
     * daemon_rpc_client 及 gateway 的 AIRY_LLM_TCP_ADDR/PORT 约定一致。 */
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
        return NULL;

    char host[128];
    char port_str[16];
    const char *colon = sock_path ? strrchr(sock_path, ':') : NULL;
    if (!colon || colon == sock_path || (size_t)(colon - sock_path) >= sizeof(host) ||
        strlen(colon + 1) >= sizeof(port_str)) {
        closesocket(fd);
        return NULL;
    }
    size_t host_len = (size_t)(colon - sock_path);
    AIRY_MEMCPY(host, sock_path, host_len);
    host[host_len] = '\0';
    AIRY_STRNCPY_TERM(port_str, colon + 1, sizeof(port_str));
    struct sockaddr_in addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)atoi(port_str));
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0)
        addr.sin_addr.s_addr = INADDR_LOOPBACK;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(fd);
        return NULL;
    }

    int timeout_ms_win = timeout_ms > 0 ? timeout_ms : GW_LLM_DEFAULT_TIMEOUT_MS;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms_win,
               sizeof(timeout_ms_win));

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        closesocket(fd);
        return NULL;
    }
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", method);
    if (params_json && params_json[0]) {
        cJSON *p = cJSON_Parse(params_json);
        cJSON_AddItemToObject(req, "params", p ? p : cJSON_CreateObject());
    } else {
        cJSON_AddItemToObject(req, "params", cJSON_CreateObject());
    }
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) {
        closesocket(fd);
        return NULL;
    }

    size_t len = strlen(req_str);
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, req_str + sent, (int)(len - sent), 0);
        if (n <= 0) {
            AIRY_FREE(req_str);
            closesocket(fd);
            return NULL;
        }
        sent += (size_t)n;
    }
    AIRY_FREE(req_str);

    size_t cap = 65536;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
        closesocket(fd);
        return NULL;
    }
    resp[0] = '\0';
    char buf[4096];
    for (;;) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > GW_LLM_MAX_RESP) {
                AIRY_FREE(resp);
                closesocket(fd);
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
                closesocket(fd);
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }
    closesocket(fd);
    return resp;
#endif
}

/**
 * @brief Invoke the think_d dual-thinking stage (think.process: GCCP goal
 *        confirmation + GRAD plan critique)
 *
 * The main dialog path uses dual thinking (product decision, 2026-08-07): when
 * agent.run has no agent orchestration, think_d runs GCCP+GRAD first to produce
 * a converged DAG plan and thinking events, which are injected into the LLM
 * request context and returned with the response, replacing the previous
 * gateway -> llm_d single-model direct call (D4 fix).
 *
 * @param ctx       Gateway context
 * @param prompt    User input
 * @param out_think Dual-thinking result JSON (cJSON object, caller cJSON_Delete):
 *        {plan:{...DAG...}, feedback:[...], stats:{...}}
 * @return 0 on success (*out_think valid); non-zero on failure
 *         (think_d unreachable/timed out, degraded to direct call)
 */
int gw_think_process(const gateway_business_ctx_t *ctx, const char *prompt, cJSON **out_think)
{
    *out_think = NULL;
    if (!ctx || !prompt || !*prompt)
        return -1;

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON *p = cJSON_CreateString(prompt);
    cJSON_AddItemToObject(params, "prompt", p);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    char *resp = gw_svc_call(ctx->think_sock_path, "process", params_str, GW_THINK_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp) {
        AIRY_LOG_WARN("gateway: think.process failed (think_d unreachable at %s), "
                 "degrading to direct LLM",
                 ctx->think_sock_path);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root)
        return -1;
    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        AIRY_LOG_WARN("gateway: think.process returned error");
        cJSON_Delete(root);
        return -1;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsString(result) || !result->valuestring) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *think = cJSON_Parse(result->valuestring);
    cJSON_Delete(root);
    if (!think)
        return -1;
    *out_think = think;
    AIRY_LOG_INFO("gateway: think.process ok (dual-thinking engaged)");
    return 0;
}

/**
 * @brief ACL check for tool execution from external protocols
 *
 * Fail-closed: daemon_check_tool_permission DENYs any rule not registered for
 * (agent_id, tool_name). Default rules are registered at startup in main.c
 * (fs_read/fs_write/fs_list allow; shell_run follows the
 * AIRY_GATEWAY_ACL_ALLOW_SHELL env var, deny by default).
 *
 * @param tool_name Tool name
 * @return 0 allowed, non-zero denied
 */
int gw_acl_check_tool(const char *tool_name)
{
    if (!tool_name)
        return -1;
    int rc = daemon_check_tool_permission(GW_EXTERNAL_AGENT_ID, tool_name, "execute");
    if (rc != 0) {
        AIRY_LOG_WARN("gateway ACL DENY: agent=%s tool=%s (fail-closed)", GW_EXTERNAL_AGENT_ID,
                 tool_name);
        return -1;
    }
    return 0;
}

/**
 * @brief mem.* method whitelist: returns the internal mem_d method name,
 *        NULL if not whitelisted
 *
 * Passes through mem_d's registered methods
 * (write/search/get/delete/count/evolve/health_check/get_stats) and rejects
 * all other mem.* methods, preventing arbitrary methods from reaching mem_d.
 */
const char *gw_mem_method_allowlist(const char *method)
{
    if (!method)
        return NULL;
    if (strcmp(method, "mem.write") == 0)
        return "write";
    if (strcmp(method, "mem.search") == 0)
        return "search";
    if (strcmp(method, "mem.get") == 0)
        return "get";
    if (strcmp(method, "mem.delete") == 0)
        return "delete";
    if (strcmp(method, "mem.count") == 0)
        return "count";
    if (strcmp(method, "mem.evolve") == 0)
        return "evolve";
    if (strcmp(method, "mem.health_check") == 0)
        return "health_check";
    if (strcmp(method, "mem.get_stats") == 0)
        return "get_stats";
    return NULL;
}

/* Namespace forwarding method whitelists (one per daemon, L2 methods only) */
static const char *GW_A2A_METHODS[] = {
    "register_agent", "unregister_agent", "discover_agents", "create_task", "update_task",
    "cancel_task",    "get_task",         "send_message",    "count",       "send",
    "receive",        "health_check",     "get_stats",       NULL};
static const char *GW_PLUGIN_METHODS[] = {"load",    "unload",       "start",     "stop",
                                          "execute", "get_metadata", "get_state", "get_stats",
                                          "list",    "install",      "uninstall", "health_check",
                                          NULL};
static const char *GW_INFO_METHODS[] = {"system",       "history",   "health",
                                        "health_check", "get_stats", NULL};
static const char *GW_NOTIFY_METHODS[] = {"publish", "subscribe",    "unsubscribe", "list",
                                          "health",  "health_check", "get_stats",   NULL};
static const char *GW_OBSERVE_METHODS[] = {"record_metric", "query_metrics", "get_metrics",
                                           "get_stats",     "health_check",  NULL};
static const char *GW_MARKET_METHODS[] = {"register_agent",
                                          "search_agents",
                                          "install_agent",
                                          "register_skill",
                                          "search_skills",
                                          "health_check",
                                          "publish",
                                          "search",
                                          "install",
                                          "get_stats",
                                          NULL};
static const char *GW_HOOK_METHODS[] = {"register",     "unregister", "trigger", "list",
                                        "status",       "stats",      "health",  "ping",
                                        "health_check", "get_stats",  NULL};
static const char *GW_SCHED_METHODS[] = {"register_agent",
                                         "unregister_agent",
                                         "schedule_task",
                                         "get_task",
                                         "cancel",
                                         "dag_submit",
                                         "dag_status",
                                         "dag_cancel",
                                         "checkpoint_save",
                                         "submit",
                                         "query",
                                         "get_stats",
                                         "health_check",
                                         NULL};
static const char *GW_THINK_METHODS[] = {"process", "health_check", "get_stats", NULL};
static const char *GW_MONIT_METHODS[] = {"record_metric", "get_metrics",  "trigger_alert",
                                         "get_alerts",    "health_check", "generate_report",
                                         "heartbeat",     "metrics",      "alert_raise",
                                         "alert_resolve", "get_stats",    NULL};
static const char *GW_CHANNEL_METHODS[] = {"ping",   "list",         "open",      "close", "send",
                                           "health", "health_check", "get_stats", NULL};
static const char *GW_CUPOLAS_METHODS[] = {"check_permission",   "sanitize",
                                           "execute_command",    "add_rule",
                                           "audit_flush",        "health_check",
                                           "get_stats",          "vault_store",
                                           "vault_retrieve",     "vault_delete",
                                           "vault_list",         "vault_rotate",
                                           "net_add_rule",       "net_check_access",
                                           "net_get_stats",      "entitlements_load",
                                           "entitlements_check", NULL};
static const char *GW_AGENT_METHODS[] = {"spawn", "terminate",    "invoke",    "cancel", "list",
                                         "count", "health_check", "get_stats", NULL};
static const char *GW_LLM_METHODS[] = {"complete",     "list_models", "count_tokens",
                                       "health_check", "get_stats",   NULL};
static const char *GW_TOOL_METHODS[] = {"register",     "list_tools", "get_tool",
                                        "execute_tool", "execute",    "list",
                                        "health_check", "get_stats",  NULL};

/* Namespace forwarding rules referenced by the main dispatcher */
const gw_ns_forward_rule_t GW_NS_LLM = {"llm.", NULL, GW_LLM_METHODS, GW_LLM_DEFAULT_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_AGENT = {"agent.", NULL, GW_AGENT_METHODS, GW_TOOL_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_TOOL = {"tool.", NULL, GW_TOOL_METHODS, GW_TOOL_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_A2A = {"a2a.", NULL, GW_A2A_METHODS, GW_LLM_DEFAULT_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_PLUGIN = {"plugin.", NULL, GW_PLUGIN_METHODS, GW_TOOL_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_INFO = {"info.", NULL, GW_INFO_METHODS, GW_TOOL_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_NOTIFY = {"notify.", NULL, GW_NOTIFY_METHODS, GW_TOOL_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_OBSERVE = {"observe.", NULL, GW_OBSERVE_METHODS,
                                            GW_TOOL_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_MARKET = {"market.", NULL, GW_MARKET_METHODS, GW_TOOL_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_HOOK = {"hook.", NULL, GW_HOOK_METHODS, GW_TOOL_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_SCHED = {"sched.", NULL, GW_SCHED_METHODS, GW_TOOL_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_THINK = {"think.", NULL, GW_THINK_METHODS, GW_THINK_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_MONIT = {"monit.", NULL, GW_MONIT_METHODS, GW_TOOL_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_CHANNEL = {"channel.", NULL, GW_CHANNEL_METHODS,
                                            GW_TOOL_TIMEOUT_MS};
const gw_ns_forward_rule_t GW_NS_CUPOLAS = {"cupolas.", NULL, GW_CUPOLAS_METHODS,
                                            GW_TOOL_TIMEOUT_MS};

/**
 * @brief Namespace method forwarding: gateway JSON-RPC <ns>.<method> ->
 *        daemon <method>
 *
 * Same pass-through mode as handle_mem_call: params/response are forwarded
 * as-is, the response id is rewritten to the request id. Methods inside the
 * whitelist are allowed, everything else returns -32601.
 *
 * @param rule Forwarding rule (ns/sock/whitelist/timeout)
 * @return Complete JSON-RPC response string from the target daemon
 *         (AIRY_MALLOC), or an error response on failure
 */
char *handle_ns_forward(cJSON *root, const gw_ns_forward_rule_t *rule)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    const char *method_str = cJSON_IsString(method) ? method->valuestring : NULL;
    if (!method_str || !rule)
        return jsonrpc_error(-32601, "Method not found", id);

    size_t ns_len = strlen(rule->ns);
    if (strncmp(method_str, rule->ns, ns_len) != 0)
        return jsonrpc_error(-32601, "Method not found", id);
    const char *inner = method_str + ns_len;

    int allow = 0;
    for (const char *const *m = rule->methods; m && *m; ++m) {
        if (strcmp(inner, *m) == 0) {
            allow = 1;
            break;
        }
    }
    if (!allow)
        return jsonrpc_error(-32601, "Method not found", id);

    cJSON *params = cJSON_GetObjectItem(root, "params");
    char *params_str = params ? cJSON_PrintUnformatted(params) : AIRY_STRDUP("{}");
    if (!params_str)
        return jsonrpc_error(-32603, "Out of memory", id);

    char *resp = gw_svc_call(rule->sock_path, inner, params_str, rule->timeout_ms);
    AIRY_FREE(params_str);
    if (!resp)
        return jsonrpc_error(-32603, "Service unreachable", id);

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot)
        return jsonrpc_error(-32603, "Service returned invalid response", id);

    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *svc_id = cJSON_GetObjectItem(rroot, "id");
    if (svc_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}

/**
 * @brief mem.* forwarding: gateway JSON-RPC -> mem_d (params/response pass-through)
 *
 * Env-gated by AIRY_GATEWAY_MEM_PUBLIC (default true: internal memory service
 * traffic passes; false disables external mem access without affecting the TUI
 * local JSONL).
 */
char *handle_mem_call(cJSON *root, const gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    const char *mem_method =
        gw_mem_method_allowlist(cJSON_IsString(method) ? method->valuestring : NULL);
    if (!mem_method) {
        return jsonrpc_error(-32601, "Method not found", id);
    }

    const char *pub = getenv("AIRY_GATEWAY_MEM_PUBLIC");
    if (pub && (strcmp(pub, "false") == 0 || strcmp(pub, "0") == 0)) {
        return jsonrpc_error(-32001, "Memory service access disabled", id);
    }

    char *params_str = NULL;
    if (params) {
        params_str = cJSON_PrintUnformatted(params);
    } else {
        params_str = AIRY_STRDUP("{}");
    }
    if (!params_str) {
        return jsonrpc_error(-32603, "Out of memory", id);
    }

    char *resp = gw_svc_call(ctx->mem_sock_path, mem_method, params_str, GW_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp) {
        return jsonrpc_error(-32603, "Memory service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "Memory service returned invalid response", id);
    }
    /* JSON-RPC 2.0 compliance: the response id must match the request id.
     * mem_d echoes the internal id=1 used by gw_svc_call; without rewriting,
     * concurrent requests cannot be correlated to their originals (client id
     * validation would fail). */
    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *mem_id = cJSON_GetObjectItem(rroot, "id");
    if (mem_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}

/**
 * @brief llm.list_models forwarding: gateway JSON-RPC -> llm_d list_models
 *
 * Returns all models from the llm_d provider registry plus
 * default_model/default_provider, for CLI/TUI model configuration (read-only,
 * no params, no API key needed). The response id is rewritten to the request
 * id (same concurrency compliance as handle_mem_call).
 */
char *handle_llm_list_models(cJSON *root, const gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");

    char *resp = gw_svc_call(ctx->llm_sock_path, "list_models", "{}", GW_LLM_DEFAULT_TIMEOUT_MS);
    if (!resp) {
        return jsonrpc_error(-32603, "LLM service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "LLM service returned invalid response", id);
    }

    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *llm_id = cJSON_GetObjectItem(rroot, "id");
    if (llm_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}

/**
 * @brief tool.pending / tool.approve forwarding: gateway JSON-RPC -> tool_d
 *
 * P0 interactive permission approval (Claude Code style permission prompt):
 * external tool.pending -> tool_d "pending"; external tool.approve ->
 * tool_d "approve". params/response pass through, the response id is rewritten
 * to the request id (same concurrency compliance as handle_mem_call).
 */
char *handle_tool_approval_call(cJSON *root, const gateway_business_ctx_t *ctx,
                                const char *tool_method)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    if (strcmp(tool_method, "approve") == 0) {
        const cJSON *req_id = params ? cJSON_GetObjectItem(params, "request_id") : NULL;
        const cJSON *decision = params ? cJSON_GetObjectItem(params, "decision") : NULL;
        if (!cJSON_IsString(req_id) || !req_id->valuestring || !req_id->valuestring[0] ||
            !cJSON_IsString(decision) || !decision->valuestring || !decision->valuestring[0]) {
            return jsonrpc_error(-32602, "Invalid params: request_id and decision required", id);
        }
    }

    char *params_str = NULL;
    if (params) {
        params_str = cJSON_PrintUnformatted(params);
    } else {
        params_str = AIRY_STRDUP("{}");
    }
    if (!params_str) {
        return jsonrpc_error(-32603, "Out of memory", id);
    }

    char *resp = gw_svc_call(ctx->tool_sock_path, tool_method, params_str, GW_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp) {
        return jsonrpc_error(-32603, "Tool service unreachable", id);
    }

    cJSON *rroot = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!rroot) {
        return jsonrpc_error(-32603, "Tool service returned invalid response", id);
    }

    cJSON *req_id = cJSON_GetObjectItem(root, "id");
    cJSON *tool_id = cJSON_GetObjectItem(rroot, "id");
    if (tool_id)
        cJSON_DeleteItemFromObject(rroot, "id");
    if (req_id && cJSON_IsString(req_id)) {
        cJSON_AddStringToObject(rroot, "id", req_id->valuestring);
    } else if (req_id && cJSON_IsNumber(req_id)) {
        cJSON_AddNumberToObject(rroot, "id", req_id->valuedouble);
    } else {
        cJSON_AddNullToObject(rroot, "id");
    }
    char *out = cJSON_PrintUnformatted(rroot);
    cJSON_Delete(rroot);
    return out;
}
