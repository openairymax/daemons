// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

// @owner: team-B
/**
 * @file gateway_business_handler.c
 * @brief 网关业务请求处理器实现（agent.run → llm_d 转发）
 *
 * SEC-017 合规：所有功能均为真实实现，无桩函数。
 * 处理链：HTTP JSON-RPC agent.run → 直连 llm_d(complete) → 返回对话结果。
 */

#include "gateway_business_handler.h"

#include "airy_memory.h"
#include "atomic_compat.h"
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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#define GW_LLM_DEFAULT_MODEL "deepseek-v4-flash"
/* LLM full-response timeout 90s: long-thinking / multi-tool_call rounds can
 * exceed 30s; the old 30s value made the gateway hit recv timeout while the
 * LLM had not yet returned, breaking the tool chain. */
#define GW_LLM_DEFAULT_TIMEOUT_MS 90000
/* think.process timeout 120s: dual thinking (GCCP probe/confirm + GRAD
 * multi-round quadruple-check) involves several LLM calls, each measured at
 * 15-25s; 120s covers the worst case. */
#define GW_THINK_TIMEOUT_MS 120000
#define GW_LLM_MAX_RESP 1048576
#define GW_LLM_DEFAULT_TCP_PORT 8080
#define GW_SESSION_ID_LEN 64
#define GW_EXTERNAL_AGENT_ID "external"

/**
 * @brief In-flight agent.run request entry
 *
 * agent.run is a synchronous blocking path (LLM round-trip + tool loop) and a
 * single run can take minutes. To support manual cancellation after a task
 * starts, the gateway keeps a registry of in-flight requests:
 *   - handle_agent_run registers an entry (session_id -> cancelled=0) and
 *     removes it on completion;
 *   - agent.cancel(session_id) sets cancelled, checked between tool-loop rounds.
 */
typedef struct gw_active_request_s {
    char session_id[GW_SESSION_ID_LEN];
    atomic_int cancelled;
    struct gw_active_request_s *next;
} gw_active_request_t;

struct gateway_business_ctx_s {
    char llm_sock_path[256];
    char llm_tcp_addr[64];
    uint16_t llm_tcp_port;
    char tool_sock_path[256];
    char agent_sock_path[256];
    char mem_sock_path[256];
    char sched_sock_path[256];
    char think_sock_path[256];
    char a2a_sock_path[256];
    char plugin_sock_path[256];
    char info_sock_path[256];
    char notify_sock_path[256];
    char observe_sock_path[256];
    char market_sock_path[256];
    char hook_sock_path[256];
    char monit_sock_path[256];
    char channel_sock_path[256];
    char cupolas_sock_path[256];
    char default_model[128];

    airy_mtx_t active_lock;
    gw_active_request_t *active_requests;

    gateway_shutdown_fn_t on_shutdown;
    void *shutdown_user_data;
};

static char *jsonrpc_error(int code, const char *msg, const cJSON *id)
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

#ifdef _WIN32
static int llm_connect_tcp(const gateway_business_ctx_t *ctx)
{
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET)
        return -1;
    struct sockaddr_in addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx->llm_tcp_port);
    inet_pton(AF_INET, ctx->llm_tcp_addr, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(fd);
        return -1;
    }
    return (int)fd;
}
#else
static int llm_connect_unix(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif

/**
 * @brief Send a JSON-RPC complete request to llm_d and read the response
 * @return Response string (AIRY_MALLOC), or NULL on failure
 */
static char *llm_call_complete(const gateway_business_ctx_t *ctx, const char *req_json)
{
    int fd;
#ifdef _WIN32
    fd = llm_connect_tcp(ctx);
#else
    fd = llm_connect_unix(ctx->llm_sock_path);
#endif
    if (fd < 0) {
        LOG_WARN("gateway handler: cannot connect to llm_d (sock=%s)", ctx->llm_sock_path);
        return NULL;
    }

#ifdef _WIN32
    int timeout_ms = GW_LLM_DEFAULT_TIMEOUT_MS;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv = {GW_LLM_DEFAULT_TIMEOUT_MS / 1000,
                         (GW_LLM_DEFAULT_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    size_t len = strlen(req_json);
    size_t sent_total = 0;
    while (sent_total < len) {
#ifdef _WIN32
        int n = send(fd, req_json + sent_total, (int)(len - sent_total), 0);
#else
        ssize_t n = send(fd, req_json + sent_total, len - sent_total, 0);
#endif
        if (n <= 0) {
#ifdef _WIN32
            closesocket(fd);
#else
            close(fd);
#endif
            return NULL;
        }
        sent_total += (size_t)n;
    }

    size_t cap = 4096;
    size_t used = 0;
    char *resp = (char *)AIRY_MALLOC(cap);
    if (!resp) {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        return NULL;
    }
    resp[0] = '\0';

    char buf[4096];
    for (;;) {
#ifdef _WIN32
        int n = recv(fd, buf, sizeof(buf), 0);
#else
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
#endif
        if (n <= 0)
            break;
        if (used + (size_t)n + 1 > cap) {
            size_t new_cap = (used + (size_t)n + 1) * 2;
            if (new_cap > GW_LLM_MAX_RESP) {
                AIRY_FREE(resp);
#ifdef _WIN32
                closesocket(fd);
#else
                close(fd);
#endif
                return NULL;
            }
            char *np = (char *)AIRY_REALLOC(resp, new_cap);
            if (!np) {
                AIRY_FREE(resp);
#ifdef _WIN32
                closesocket(fd);
#else
                close(fd);
#endif
                return NULL;
            }
            resp = np;
            cap = new_cap;
        }
        AIRY_MEMCPY(resp + used, buf, (size_t)n);
        used += (size_t)n;
        resp[used] = '\0';
    }

#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    return resp;
}

/* Builtin tool OpenAI tools schema (one-to-one with the tools registered in
 * tool_d builtin.c). All "required" fields must match the parameter sets
 * registered in tool_d: its validator treats every registered parameter as
 * mandatory, so if the schema marks one optional while tool_d requires it
 * (e.g. fs_list's path), the LLM may omit it and tool validation fails. */
static const char GW_TOOLS_JSON[] =
    "["
    "{\"type\":\"function\",\"function\":{\"name\":\"fs_read\","
    "\"description\":\"Read a file's content from the local filesystem\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}}"
    ",{\"type\":\"function\",\"function\":{\"name\":\"fs_write\","
    "\"description\":\"Write content to a local file (creates or overwrites)\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}}"
    ",{\"type\":\"function\",\"function\":{\"name\":\"fs_list\","
    "\"description\":\"List entries of a local directory (JSON array)\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}}"
    ",{\"type\":\"function\",\"function\":{\"name\":\"shell_run\","
    "\"description\":\"Execute a shell command and capture its output\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"]}}}"
    ",{\"type\":\"function\",\"function\":{\"name\":\"web_fetch\","
    "\"description\":\"Fetch a web page over HTTP(S) and return its body text\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},"
    "\"required\":[\"url\"]}}}"
    "]";

#define GW_MAX_TOOL_LOOPS 8
/* Tool execution timeout 90s: shell_run itself times out at 60s; the old 30s
 * value would hit recv timeout before the tool finished, making the gateway
 * wrongly report long commands as failed. */
#define GW_TOOL_TIMEOUT_MS 90000

#define GW_AGENT_SPAWN_TIMEOUT_MS 90000
#define GW_AGENT_INVOKE_TIMEOUT_MS 180000

/**
 * @brief Send a JSON-RPC request to tool_d and read the response (POSIX Unix socket)
 * @return Response string (AIRY_MALLOC), or NULL on failure
 */
static char *tool_call_rpc(const gateway_business_ctx_t *ctx, const char *req_json)
{
#ifndef _WIN32
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;
    struct sockaddr_un addr;
    AIRY_MEMSET(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    AIRY_STRNCPY_TERM(addr.sun_path, ctx->tool_sock_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_WARN("gateway handler: cannot connect to tool_d (sock=%s)", ctx->tool_sock_path);
        close(fd);
        return NULL;
    }

    struct timeval tv = {GW_TOOL_TIMEOUT_MS / 1000, (GW_TOOL_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t len = strlen(req_json);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req_json + sent, len - sent, 0);
        if (n <= 0) {
            close(fd);
            return NULL;
        }
        sent += (size_t)n;
    }

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
    (void)ctx;
    (void)req_json;
    return NULL;
#endif
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
static char *gw_svc_call(const char *sock_path, const char *method, const char *params_json,
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
    (void)sock_path;
    (void)method;
    (void)params_json;
    (void)timeout_ms;
    return NULL;
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
static int gw_think_process(const gateway_business_ctx_t *ctx, const char *prompt,
                            cJSON **out_think)
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
        LOG_WARN("gateway: think.process failed (think_d unreachable at %s), "
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
        LOG_WARN("gateway: think.process returned error");
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
    LOG_INFO("gateway: think.process ok (dual-thinking engaged)");
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
static int gw_acl_check_tool(const char *tool_name)
{
    if (!tool_name)
        return -1;
    int rc = daemon_check_tool_permission(GW_EXTERNAL_AGENT_ID, tool_name, "execute");
    if (rc != 0) {
        LOG_WARN("gateway ACL DENY: agent=%s tool=%s (fail-closed)", GW_EXTERNAL_AGENT_ID,
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
static const char *gw_mem_method_allowlist(const char *method)
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

/* ==================== Generic namespace forwarding (a2a./plugin./info./notify./observe./market./hook.)
 * ====================
 */

/* Namespace forwarding rule: <ns>.<method> -> target daemon <method> (whitelist).
 * Only L2 methods registered by each daemon are allowed
 * (02-l2-service-protocol.md §6), preventing arbitrary method pass-through.
 * Sensitive methods such as execute must be listed explicitly by the caller. */
typedef struct {
    const char *ns;
    const char *sock_path;
    const char *const *methods;
    int timeout_ms;
} gw_ns_forward_rule_t;

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
static char *handle_ns_forward(cJSON *root, const gw_ns_forward_rule_t *rule)
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
static char *handle_mem_call(cJSON *root, const gateway_business_ctx_t *ctx)
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
static char *handle_llm_list_models(cJSON *root, const gateway_business_ctx_t *ctx)
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
static char *handle_tool_approval_call(cJSON *root, const gateway_business_ctx_t *ctx,
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

/**
 * @brief Generate a unique session ID (time + incrementing counter + random
 *        bits, avoiding time(NULL) pseudo-sessions)
 */
static void gw_gen_session_id(char *out, size_t out_size)
{
    static uint64_t seq = 0;
    uint64_t now = (uint64_t)airy_time_ms();
    uint64_t s = seq++;
    uint64_t rand_bits = 0;
    {

        uint64_t *p = (uint64_t *)&now;
        rand_bits = ((*p) ^ (s << 32)) * 6364136223846793005ULL;
    }
    snprintf(out, out_size, "sess_%016llx_%04llx", (unsigned long long)(now ^ rand_bits),
             (unsigned long long)(s & 0xFFFF));
}

/**
 * @brief Register an in-flight request (cancelled=0)
 */
static gw_active_request_t *gw_active_register(gateway_business_ctx_t *ctx, const char *session_id)
{
    gw_active_request_t *entry = (gw_active_request_t *)AIRY_CALLOC(1, sizeof(gw_active_request_t));
    if (!entry)
        return NULL;
    AIRY_STRNCPY_TERM(entry->session_id, session_id, sizeof(entry->session_id));
    atomic_store_explicit(&entry->cancelled, 0, memory_order_relaxed);
    airy_mtx_lock(&ctx->active_lock);
    entry->next = ctx->active_requests;
    ctx->active_requests = entry;
    airy_mtx_unlock(&ctx->active_lock);
    LOG_INFO("gateway: agent.run registered (session=%s)", entry->session_id);
    return entry;
}

/**
 * @brief Unregister an in-flight request
 */
static void gw_active_unregister(gateway_business_ctx_t *ctx, gw_active_request_t *entry)
{
    if (!entry)
        return;
    airy_mtx_lock(&ctx->active_lock);
    gw_active_request_t **pp = (gw_active_request_t **)&ctx->active_requests;
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            break;
        }
        pp = &(*pp)->next;
    }
    airy_mtx_unlock(&ctx->active_lock);
    LOG_INFO("gateway: agent.run unregistered (session=%s)", entry->session_id);
    AIRY_FREE(entry);
}

/**
 * @brief Whether a cancellation has been requested
 */
static bool gw_active_is_cancelled(gw_active_request_t *entry)
{
    return entry && atomic_load_explicit(&entry->cancelled, memory_order_relaxed) != 0;
}

/**
 * @brief Agent orchestration path: spawn + invoke (params.agent -> agent_d)
 * Calls agent_d's spawn(agent_spec) -> gets agent_id -> invoke(input=prompt)
 * -> returns output. Agent lifecycle is managed by agent_d (idle agents are
 * reaped automatically, see agent_service_reap_idle); the gateway holds no
 * agent state.
 *
 * @param ctx        Gateway context (contains agent_sock_path)
 * @param agent_spec params.agent (JSON object with role/language fields)
 * @param prompt     User input (used as invoke input)
 * @param out_text   Final output (AIRY_MALLOC, caller AIRY_FREE)
 * @param out_err    Failure reason (AIRY_MALLOC, caller AIRY_FREE; NULL on success)
 * @return 0 on success, non-zero on failure
 */
static int gw_agent_run_orchestrate(const gateway_business_ctx_t *ctx, const cJSON *agent_spec,
                                    const char *prompt, char **out_text, char **out_err)
{
    *out_text = NULL;
    *out_err = NULL;
    if (!cJSON_IsObject(agent_spec)) {
        *out_err = AIRY_STRDUP("params.agent must be a JSON object (role/language/...)");
        return -1;
    }

    char *spec_str = cJSON_PrintUnformatted(agent_spec);
    if (!spec_str) {
        *out_err = AIRY_STRDUP("cannot serialize params.agent");
        return -1;
    }

    size_t spawn_n = strlen(spec_str) + 32;
    char *spawn_params = (char *)AIRY_MALLOC(spawn_n);
    if (!spawn_params) {
        AIRY_FREE(spec_str);
        *out_err = AIRY_STRDUP("out of memory");
        return -1;
    }
    snprintf(spawn_params, spawn_n, "{\"agent_spec\":%s}", spec_str);
    AIRY_FREE(spec_str);

    char *spawn_resp =
        gw_svc_call(ctx->agent_sock_path, "spawn", spawn_params, GW_AGENT_SPAWN_TIMEOUT_MS);
    AIRY_FREE(spawn_params);
    if (!spawn_resp) {
        *out_err = AIRY_STRDUP("agent_d unreachable (spawn)");
        return -1;
    }

    char *agent_id = NULL;
    cJSON *sroot = cJSON_Parse(spawn_resp);
    if (sroot) {
        cJSON *err = cJSON_GetObjectItem(sroot, "error");
        cJSON *result = err ? NULL : cJSON_GetObjectItem(sroot, "result");
        cJSON *aid = result ? cJSON_GetObjectItem(result, "agent_id") : NULL;
        cJSON *err_msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
        if (err && cJSON_IsString(err_msg) && err_msg->valuestring) {
            *out_err = AIRY_STRDUP(err_msg->valuestring);
        } else if (cJSON_IsString(aid) && aid->valuestring) {
            agent_id = AIRY_STRDUP(aid->valuestring);
        } else {
            *out_err = AIRY_STRDUP("agent.spawn returned no agent_id");
        }
    } else {
        *out_err = AIRY_STRDUP("agent.spawn returned invalid response");
    }
    cJSON_Delete(sroot);
    AIRY_FREE(spawn_resp);
    if (!agent_id) {
        if (!*out_err)
            *out_err = AIRY_STRDUP("agent.spawn failed");
        return -1;
    }

    cJSON *invoke_params = cJSON_CreateObject();
    if (!invoke_params) {
        AIRY_FREE(agent_id);
        *out_err = AIRY_STRDUP("out of memory");
        return -1;
    }
    cJSON_AddStringToObject(invoke_params, "agent_id", agent_id);
    cJSON_AddStringToObject(invoke_params, "input", prompt ? prompt : "");
    char *invoke_params_str = cJSON_PrintUnformatted(invoke_params);
    cJSON_Delete(invoke_params);
    AIRY_FREE(agent_id);
    if (!invoke_params_str) {
        *out_err = AIRY_STRDUP("out of memory");
        return -1;
    }

    char *invoke_resp =
        gw_svc_call(ctx->agent_sock_path, "invoke", invoke_params_str, GW_AGENT_INVOKE_TIMEOUT_MS);
    AIRY_FREE(invoke_params_str);
    if (!invoke_resp) {
        *out_err = AIRY_STRDUP("agent_d unreachable (invoke)");
        return -1;
    }

    int rc = -1;
    cJSON *iroot = cJSON_Parse(invoke_resp);
    if (iroot) {
        cJSON *err = cJSON_GetObjectItem(iroot, "error");
        cJSON *result = err ? NULL : cJSON_GetObjectItem(iroot, "result");
        cJSON *out = result ? cJSON_GetObjectItem(result, "output") : NULL;
        cJSON *err_msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
        if (err && cJSON_IsString(err_msg) && err_msg->valuestring) {
            *out_err = AIRY_STRDUP(err_msg->valuestring);
        } else if (cJSON_IsString(out)) {
            *out_text = AIRY_STRDUP(out->valuestring);
            rc = 0;
        } else {
            *out_err = AIRY_STRDUP("agent.invoke returned no output");
        }
    } else {
        *out_err = AIRY_STRDUP("agent.invoke returned invalid response");
    }
    cJSON_Delete(iroot);
    AIRY_FREE(invoke_resp);
    return rc;
}

/* ==================== agent_file → agent spec ==================== */
#define GW_AGENT_FILE_MAX (64 * 1024)

/* Find the top-level "role:" key in YAML-like content and return the start of
 * its value (skipping the colon and leading whitespace); NULL if not found.
 * Supports only the simplest key-value form; complex YAML should use JSON. */
static const char *yaml_role_value(const char *buf)
{
    const char *p = buf;
    while ((p = strstr(p, "role")) != NULL) {
        const char *q = p + 4;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q == ':')
            return q + 1;
        p = p + 4;
    }
    return NULL;
}

/*
 * Parse an agent spec from params.agent_file (fallback when params.agent is
 * not provided directly):
 *   1. JSON file (e.g. {"role":"coding","language":"python"}) -> the whole
 *      object is used as the spec;
 *   2. Simple YAML (role: xxx line) -> extract role into {"role": xxx};
 *   3. Other plain text -> truncate the leading part as role.
 * Returns a newly allocated cJSON object (caller cJSON_Delete); NULL when no
 * valid spec is found.
 *
 * Design trade-off: the orchestration branch (spawn+invoke) only needs role to
 * derive an agent, so only the role field is parsed here; full declarations
 * such as language/capabilities are future extensions, not over-designed now.
 */
static cJSON *gw_agent_spec_from_agent_file(const cJSON *params)
{
    cJSON *af = cJSON_GetObjectItem(params, "agent_file");
    if (!cJSON_IsString(af) || !af->valuestring || !*af->valuestring)
        return NULL;

    FILE *f = fopen(af->valuestring, "rb");
    if (!f) {
        LOG_WARN("gateway: agent_file unreadable: %s", af->valuestring);
        return NULL;
    }
    char *buf = (char *)AIRY_MALLOC(GW_AGENT_FILE_MAX + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, GW_AGENT_FILE_MAX, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *parsed = cJSON_Parse(buf);
    if (parsed) {
        if (cJSON_IsObject(parsed)) {
            AIRY_FREE(buf);
            return parsed;
        }
        cJSON_Delete(parsed);
    }

    const char *src = yaml_role_value(buf);
    if (!src)
        src = buf;
    size_t start = 0;
    while (src[start] == ' ' || src[start] == '\t')
        start++;
    size_t end = start;
    while (src[end] && src[end] != '\n' && src[end] != '\r' && src[end] != ',' &&
           end < GW_AGENT_FILE_MAX && end - start < 127)
        end++;
    while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t' || src[end - 1] == '"' ||
                           src[end - 1] == '\''))
        end--;

    cJSON *spec = NULL;
    if (end > start) {
        spec = cJSON_CreateObject();
        if (spec) {
            char role[128];
            size_t rlen = end - start;

            AIRY_MEMCPY(role, src + start, rlen);
            role[rlen] = '\0';
            cJSON_AddStringToObject(spec, "role", role);
            LOG_INFO("gateway: agent spec built from agent_file (role=%s, file=%s)", role,
                     af->valuestring);
        }
    } else {
        LOG_WARN("gateway: agent_file contains no usable role: %s", af->valuestring);
    }
    AIRY_FREE(buf);
    return spec;
}

/**
 * @brief Extract reply text and token usage from the llm response
 * @return 0 on success (*out_text / *out_tokens / *out_cost valid), non-zero on failure
 */
static int parse_llm_result(const char *llm_resp, char **out_text, uint64_t *out_tokens,
                            double *out_cost)
{
    *out_text = NULL;
    *out_tokens = 0;
    if (out_cost)
        *out_cost = 0.0;

    cJSON *root = cJSON_Parse(llm_resp);
    if (!root)
        return -1;

    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        LOG_WARN("gateway handler: llm_d error: %s", cJSON_IsString(msg) ? msg->valuestring : "?");
        cJSON_Delete(root);
        return -1;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 =
        (choices && cJSON_GetArraySize(choices) > 0) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *content = choice0 ? cJSON_GetObjectItem(choice0, "content") : NULL;
    if (cJSON_IsString(content)) {
        *out_text = AIRY_STRDUP(content->valuestring);
    } else {
        /* In tool-call rounds the LLM content is usually null (response holds
         * only tool_calls); fall back to an empty string instead of failing so
         * the tool loop can continue. */
        *out_text = AIRY_STRDUP("");
    }

    /* tokens: prefer usage.total_tokens (OpenAI compatible), then top-level
     * total_tokens (llm_d legacy format); if both are missing, sum
     * prompt+completion inside usage. */
    cJSON *usage = result ? cJSON_GetObjectItem(result, "usage") : NULL;
    cJSON *total = usage ? cJSON_GetObjectItem(usage, "total_tokens") : NULL;
    if (!cJSON_IsNumber(total)) {
        total = result ? cJSON_GetObjectItem(result, "total_tokens") : NULL;
    }
    if (!cJSON_IsNumber(total) && cJSON_IsObject(usage)) {
        cJSON *u_pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON *u_ct = cJSON_GetObjectItem(usage, "completion_tokens");
        if (cJSON_IsNumber(u_pt) || cJSON_IsNumber(u_ct)) {
            uint64_t sum = 0;
            if (cJSON_IsNumber(u_pt))
                sum += (uint64_t)(u_pt->valuedouble > 0 ? u_pt->valuedouble : 0);
            if (cJSON_IsNumber(u_ct))
                sum += (uint64_t)(u_ct->valuedouble > 0 ? u_ct->valuedouble : 0);
            *out_tokens = sum;
        }
    } else if (cJSON_IsNumber(total)) {
        *out_tokens = (uint64_t)(total->valuedouble > 0 ? total->valuedouble : 0);
    }

    if (out_cost) {
        cJSON *cost = result ? cJSON_GetObjectItem(result, "cost_usd") : NULL;
        if (cJSON_IsNumber(cost))
            *out_cost = cost->valuedouble;
    }

    cJSON_Delete(root);
    return 0;
}

/**
 * @brief Extract tool_calls from the llm_d response (choices[0].tool_calls)
 * @return 0 with tool_calls present (*out caller cJSON_Delete), non-zero without
 */
static int parse_llm_tool_calls(const char *llm_resp, cJSON **out_tool_calls)
{
    *out_tool_calls = NULL;
    cJSON *root = cJSON_Parse(llm_resp);
    if (!root)
        return -1;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 =
        (choices && cJSON_GetArraySize(choices) > 0) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *tc = choice0 ? cJSON_GetObjectItem(choice0, "tool_calls") : NULL;
    if (cJSON_IsArray(tc) && cJSON_GetArraySize(tc) > 0) {
        *out_tool_calls = cJSON_Duplicate(tc, 1);
    }
    cJSON_Delete(root);
    return *out_tool_calls ? 0 : -1;
}

/**
 * @brief Execute a single tool via tool_d execute_tool
 * @param name      Tool name (fs_read/fs_write/fs_list/shell_run)
 * @param args_json Tool args (OpenAI tool_call arguments JSON string)
 * @param out_text  Result text (AIRY_MALLOC, caller frees); error description on failure
 * @return 0 on success, non-zero on failure
 */
static int gw_execute_tool(const gateway_business_ctx_t *ctx, const char *name,
                           const char *args_json, char **out_text)
{
    *out_text = NULL;

    if (gw_acl_check_tool(name) != 0) {
        *out_text = AIRY_STRDUP("Permission denied: tool not authorized");
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    if (!req)
        return -1;
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", 1);
    cJSON_AddStringToObject(req, "method", "execute_tool");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "tool_id", name);
    cJSON *pargs = cJSON_Parse(args_json && args_json[0] ? args_json : "{}");
    if (!pargs)
        pargs = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "params", pargs);
    cJSON_AddItemToObject(req, "params", params);
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str)
        return -1;

    char *resp = tool_call_rpc(ctx, req_str);
    AIRY_FREE(req_str);
    if (!resp) {
        *out_text = AIRY_STRDUP("Tool service unreachable");
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root) {
        *out_text = AIRY_STRDUP("Tool service returned invalid response");
        return -1;
    }

    /* Build result text: prefer output; on error "Error: <error>".
     * Return code semantics: 0 = tool executed successfully, non-zero = tool
     * layer failure (RPC succeeded but the tool errored/raised). */
    char *text = NULL;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *err = cJSON_GetObjectItem(root, "error");
    int tool_ok = 0;
    if (result) {
        cJSON *success = cJSON_GetObjectItem(result, "success");
        cJSON *output = cJSON_GetObjectItem(result, "output");
        cJSON *error = cJSON_GetObjectItem(result, "error");
        tool_ok = cJSON_IsNumber(success) && success->valueint != 0;
        if (tool_ok) {
            text = AIRY_STRDUP(cJSON_IsString(output) && output->valuestring ? output->valuestring :
                                                                               "(no output)");
        } else {
            const char *e = cJSON_IsString(error) && error->valuestring ? error->valuestring :
                                                                          "execution failed";
            size_t elen = strlen(e) + 8;
            text = (char *)AIRY_MALLOC(elen);
            if (text)
                snprintf(text, elen, "Error: %s", e);
        }
    } else if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        const char *m = cJSON_IsString(msg) && msg->valuestring ? msg->valuestring : "unknown";
        size_t elen = strlen(m) + 8;
        text = (char *)AIRY_MALLOC(elen);
        if (text)
            snprintf(text, elen, "Error: %s", m);
    }
    if (!text)
        text = AIRY_STRDUP("Tool execution returned no result");
    *out_text = text;
    cJSON_Delete(root);
    return tool_ok ? 0 : -1;
}

/**
 * @brief Build the llm_d complete JSON-RPC request (passes through the tools array)
 * @param model    Model name
 * @param messages Conversation history (cJSON array, deep-copied into the request)
 * @return JSON request string (AIRY_MALLOC), or NULL on failure
 */
static char *gw_build_llm_request(const char *model, const cJSON *messages)
{
    cJSON *llm_req = cJSON_CreateObject();
    if (!llm_req)
        return NULL;
    cJSON_AddStringToObject(llm_req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(llm_req, "id", 1);
    cJSON_AddStringToObject(llm_req, "method", "complete");
    cJSON *llm_params = cJSON_CreateObject();
    if (!llm_params) {
        cJSON_Delete(llm_req);
        return NULL;
    }
    cJSON_AddStringToObject(llm_params, "model", model);
    cJSON_AddItemToObject(llm_params, "messages", cJSON_Duplicate(messages, 1));
    cJSON *tools = cJSON_Parse(GW_TOOLS_JSON);
    if (tools) {
        cJSON_AddItemToObject(llm_params, "tools", tools);
    }
    cJSON_AddNumberToObject(llm_params, "max_tokens", 2048);
    cJSON_AddNumberToObject(llm_params, "temperature", 0.7);
    cJSON_AddItemToObject(llm_req, "params", llm_params);

    char *req_str = cJSON_PrintUnformatted(llm_req);
    cJSON_Delete(llm_req);
    return req_str;
}

/**
 * @brief Run the agent tool loop: LLM -> tool_calls -> execute -> feed back ->
 *        continue (ReAct)
 *
 * The full reasoning + tool chain of a single agent.run. The loop is capped at
 * GW_MAX_TOOL_LOOPS rounds to avoid runaway; after each round the assistant
 * (with tool_calls) and tool (with tool_call_id) messages are appended to the
 * conversation history for the next LLM round.
 *
 * @param ctx       Gateway context
 * @param model     Model name
 * @param prompt    User input
 * @param history   Full conversation history (OpenAI messages array, may be
 *        NULL). When non-empty it seeds the tool loop (true multi-turn context
 *        across requests, M2 fix) with the last user message as the current
 *        input; empty/invalid history degrades to a single prompt message.
 * @param active    In-flight request entry (agent.cancel support; the cancel
 *        flag is checked between rounds)
 * @param out_trace Tool trace array (cJSON, caller cJSON_Delete; NULL on failure)
 * @param out_text  Final reply text (AIRY_MALLOC, caller AIRY_FREE; NULL on failure)
 * @param out_tokens Cumulative token usage
 * @param out_cost  Cumulative cost (USD)
 * @return 0 success (*out_text valid); 1 user cancelled; non-zero failure
 *         (no final answer obtained)
 */
static int gw_run_tool_loop(const gateway_business_ctx_t *ctx, const char *model,
                            const char *prompt, const cJSON *history, gw_active_request_t *active,
                            cJSON **out_trace, char **out_text, uint64_t *out_tokens,
                            double *out_cost)
{
    *out_trace = NULL;
    *out_text = NULL;
    *out_tokens = 0;
    if (out_cost)
        *out_cost = 0.0;

    cJSON *messages = NULL;
    if (history && cJSON_IsArray(history) && cJSON_GetArraySize(history) > 0) {

        messages = cJSON_Duplicate(history, 1);
    }
    if (!messages) {
        messages = cJSON_CreateArray();
        cJSON *msg0 = cJSON_CreateObject();
        cJSON_AddStringToObject(msg0, "role", "user");
        cJSON_AddStringToObject(msg0, "content", prompt);
        cJSON_AddItemToArray(messages, msg0);
    }
    cJSON *tool_trace = cJSON_CreateArray();
    if (!messages || !tool_trace) {
        if (messages)
            cJSON_Delete(messages);
        if (tool_trace)
            cJSON_Delete(tool_trace);
        return -1;
    }

    char *final_text = NULL;
    uint64_t total_tokens = 0;
    double total_cost = 0.0;
    int rc = -1;

    for (int loops = 0; loops < GW_MAX_TOOL_LOOPS; loops++) {

        if (gw_active_is_cancelled(active)) {
            LOG_INFO("gateway: agent.run cancelled by user (session=%s)",
                     active ? active->session_id : "?");
            rc = 1;
            break;
        }

        char *llm_req_str = gw_build_llm_request(model, messages);
        if (!llm_req_str) {
            break;
        }
        char *llm_resp = llm_call_complete(ctx, llm_req_str);
        AIRY_FREE(llm_req_str);
        if (!llm_resp) {
            break;
        }

        cJSON *tool_calls = NULL;
        parse_llm_tool_calls(llm_resp, &tool_calls);

        char *text = NULL;
        uint64_t tokens = 0;
        double cost = 0.0;
        parse_llm_result(llm_resp, &text, &tokens, &cost);
        total_tokens += tokens;
        total_cost += cost;

        cJSON *assistant_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(assistant_msg, "role", "assistant");
        cJSON_AddStringToObject(assistant_msg, "content", text ? text : "");
        if (tool_calls) {
            cJSON_AddItemToObject(assistant_msg, "tool_calls", cJSON_Duplicate(tool_calls, 1));
        }
        cJSON_AddItemToArray(messages, assistant_msg);

        if (!tool_calls) {

            final_text = text ? text : AIRY_STRDUP("");
            AIRY_FREE(llm_resp);
            rc = 0;
            break;
        }

        AIRY_FREE(text);

        int tc_count = cJSON_GetArraySize(tool_calls);
        for (int i = 0; i < tc_count; i++) {
            cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *fn_name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            cJSON *fn_args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
            cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
            const char *tname = cJSON_IsString(fn_name) ? fn_name->valuestring : "";
            const char *targs = cJSON_IsString(fn_args) ? fn_args->valuestring : "{}";
            const char *tid = cJSON_IsString(tc_id) ? tc_id->valuestring : "";

            char *result_text = NULL;
            int erc = gw_execute_tool(ctx, tname, targs, &result_text);

            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", tid);
            cJSON_AddStringToObject(tool_msg, "content",
                                    result_text ? result_text : "Tool execution failed");
            cJSON_AddItemToArray(messages, tool_msg);

            cJSON *tr = cJSON_CreateObject();
            cJSON_AddStringToObject(tr, "tool", tname);
            cJSON_AddStringToObject(tr, "arguments", targs);
            cJSON_AddStringToObject(tr, "result", result_text ? result_text : "");
            cJSON_AddNumberToObject(tr, "ok", erc == 0 ? 1 : 0);
            cJSON_AddItemToArray(tool_trace, tr);

            if (result_text)
                AIRY_FREE(result_text);
        }

        cJSON_Delete(tool_calls);
        AIRY_FREE(llm_resp);
    }

    cJSON_Delete(messages);

    if (rc == 0) {
        *out_trace = tool_trace;
        *out_text = final_text;
        *out_tokens = total_tokens;
        if (out_cost)
            *out_cost = total_cost;
    } else {
        if (final_text)
            AIRY_FREE(final_text);
        cJSON_Delete(tool_trace);
    }
    return rc;
}

/**
 * @brief Persist the current Q&A round to mem_d after the conversation (mem.write)
 *
 * M6 fix: mem_d previously had no business caller (dangling service). After
 * each successful agent.run, the gateway writes the user prompt + assistant
 * reply as one memory record to mem_d (metadata carries session_id/role) so
 * mem.search can recall it later.
 *
 * Called directly over the internal socket only (bypasses the
 * AIRY_GATEWAY_MEM_PUBLIC gate — that is the access switch for external mem.*
 * methods; internal persistence is unaffected). Failure only warns and never
 * blocks the response.
 */
static void gw_persist_conversation(const gateway_business_ctx_t *ctx, const char *session_id,
                                    const char *user_prompt, const char *assistant_text)
{

    if (!ctx || !ctx->mem_sock_path[0] || !user_prompt)
        return;

    size_t data_len = strlen(user_prompt) + strlen(assistant_text) + 24;
    char *data = (char *)AIRY_MALLOC(data_len);
    if (!data)
        return;
    snprintf(data, data_len, "user: %s\nassistant: %s", user_prompt,
             assistant_text ? assistant_text : "");

    cJSON *wparams = cJSON_CreateObject();
    cJSON *metadata = cJSON_CreateObject();
    if (!wparams || !metadata) {
        cJSON_Delete(wparams);
        cJSON_Delete(metadata);
        AIRY_FREE(data);
        return;
    }
    cJSON_AddStringToObject(wparams, "data", data);
    cJSON_AddStringToObject(metadata, "session_id", session_id ? session_id : "");
    cJSON_AddStringToObject(metadata, "role", "agentrt");
    cJSON_AddItemToObject(wparams, "metadata", metadata);
    char *params_str = cJSON_PrintUnformatted(wparams);
    cJSON_Delete(wparams);
    AIRY_FREE(data);
    if (!params_str)
        return;

    char *resp = gw_svc_call(ctx->mem_sock_path, "write", params_str, GW_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp) {
        LOG_WARN("gateway: mem.write failed (mem_d unreachable, session=%s)",
                 session_id ? session_id : "?");
        return;
    }
    AIRY_FREE(resp);
    LOG_INFO("gateway: conversation persisted to mem_d (session=%s)",
             session_id ? session_id : "?");
}

static char *handle_agent_run(cJSON *root, gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    const char *prompt = NULL;
    if (params) {
        cJSON *p = cJSON_GetObjectItem(params, "prompt");
        if (cJSON_IsString(p)) {
            prompt = p->valuestring;
        } else {
            cJSON *messages = cJSON_GetObjectItem(params, "messages");
            cJSON *m0 = (messages && cJSON_GetArraySize(messages) > 0) ?
                            cJSON_GetArrayItem(messages, 0) :
                            NULL;
            cJSON *c = m0 ? cJSON_GetObjectItem(m0, "content") : NULL;
            if (cJSON_IsString(c))
                prompt = c->valuestring;
        }
    }
    if (!prompt || !*prompt) {
        return jsonrpc_error(-32602, "Invalid params: missing prompt", id);
    }

    const char *model = ctx->default_model;
    if (params) {
        cJSON *m = cJSON_GetObjectItem(params, "model");
        if (cJSON_IsString(m) && m->valuestring && *m->valuestring)
            model = m->valuestring;
    }

    /* Full conversation history (OpenAI messages array, optional): when
     * non-empty it seeds the tool loop (M1/M2 fix — real multi-turn context
     * across requests); empty history degrades to a single prompt. */
    cJSON *history = params ? cJSON_GetObjectItem(params, "messages") : NULL;
    if (history && (!cJSON_IsArray(history) || cJSON_GetArraySize(history) == 0)) {
        history = NULL;
    }

    /* Branch: params.agent present -> agent_d orchestration (spawn+invoke);
     * otherwise keep the llm_d direct tool loop (backward compatible, D4). */
    cJSON *tool_trace = NULL;
    char *final_text = NULL;
    uint64_t total_tokens = 0;
    double total_cost = 0.0;

    /* Session ID: the client may pre-assign one (agent.cancel needs to know
     * session_id before the request); otherwise the gateway generates a unique
     * ID (time + counter + random bits, not a time(NULL) pseudo-session). */
    char session_id[GW_SESSION_ID_LEN];
    cJSON *sid_param = params ? cJSON_GetObjectItem(params, "session_id") : NULL;
    if (cJSON_IsString(sid_param) && sid_param->valuestring && *sid_param->valuestring &&
        strlen(sid_param->valuestring) < GW_SESSION_ID_LEN &&
        strncmp(sid_param->valuestring, "sess_", 5) == 0) {
        AIRY_STRNCPY_TERM(session_id, sid_param->valuestring, sizeof(session_id));
    } else {
        gw_gen_session_id(session_id, sizeof(session_id));
    }

    gw_active_request_t *active = gw_active_register(ctx, session_id);

    cJSON *agent_spec = params ? cJSON_GetObjectItem(params, "agent") : NULL;
    /* When params.agent is not provided, fall back to parsing
     * params.agent_file to build the spec: keeps old clients that only pass
     * agent_file working, so the orchestration branch still triggers.
     * agent_spec_owned must be freed at every exit of this function (unlike
     * agent_spec which points into params). */
    cJSON *agent_spec_owned = NULL;
    if (!agent_spec && params) {
        agent_spec_owned = gw_agent_spec_from_agent_file(params);
        agent_spec = agent_spec_owned;
    }
    LOG_INFO("gateway: agent.run start (session=%s, model=%s, orchestrate=%d)", session_id,
             model ? model : "(default)", agent_spec ? 1 : 0);
    int run_rc = -1;

    cJSON *think_result = NULL;
    if (agent_spec) {
        char *err_msg = NULL;
        run_rc = gw_agent_run_orchestrate(ctx, agent_spec, prompt, &final_text, &err_msg);
        if (run_rc != 0) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Agent orchestration failed: %s",
                     err_msg ? err_msg : "unknown error");
            AIRY_FREE(err_msg);
            gw_active_unregister(ctx, active);
            if (agent_spec_owned)
                cJSON_Delete(agent_spec_owned);
            return jsonrpc_error(-32603, msg, id);
        }
        /* On the orchestration path the tool trace is produced internally by
         * the runner (ecosystem/agents), invisible to the gateway; set
         * tool_trace to an empty array to keep the field contract. */
        tool_trace = cJSON_CreateArray();
    } else {
        /* The main dialog path uses dual thinking (D4 fix, 2026-08-07):
         * without agent orchestration, think_d runs GCCP (fact-lock goal
         * confirmation) + GRAD (logic-lock plan quadruple-check/final ruling)
         * first, producing a converged DAG plan and thinking events; the plan
         * is then injected into the LLM request context (system message) so
         * the LLM answers/executes according to the plan. If think_d is
         * unreachable/timed out, degrade to the original direct call (dialog
         * availability is unaffected). */
        if (gw_think_process(ctx, prompt, &think_result) == 0 && think_result) {
            cJSON *plan = cJSON_GetObjectItem(think_result, "plan");
            if (plan) {
                char *plan_str = cJSON_PrintUnformatted(plan);
                if (plan_str) {
                    /* Build a system message carrying the dual-thinking plan
                     * and insert it at the head of messages: the LLM structures
                     * its answer around the GCCP+GRAD converged DAG plan,
                     * avoiding made-up steps. */
                    cJSON *messages_with_plan = NULL;
                    if (history && cJSON_IsArray(history) && cJSON_GetArraySize(history) > 0) {
                        messages_with_plan = cJSON_Duplicate(history, 1);
                    } else {
                        messages_with_plan = cJSON_CreateArray();
                    }
                    cJSON *sys = cJSON_CreateObject();
                    char sys_content[8192];
                    int sn = snprintf(sys_content, sizeof(sys_content),
                                      "You are executing a task under the AgentRT "
                                      "dual-thinking system (GCCP goal confirmation + "
                                      "GRAD plan critique). A verified action plan has "
                                      "been produced. Follow this DAG plan strictly:\n%s",
                                      plan_str);
                    if (sn > 0 && sn < (int)sizeof(sys_content))
                        cJSON_AddStringToObject(sys, "content", sys_content);
                    else
                        cJSON_AddStringToObject(sys, "content",
                                                "Execute the user request following "
                                                "the verified action plan.");
                    cJSON_AddStringToObject(sys, "role", "system");
                    cJSON_AddItemToArray(messages_with_plan, sys);

                    cJSON *usr = cJSON_CreateObject();
                    cJSON_AddStringToObject(usr, "role", "user");
                    cJSON_AddStringToObject(usr, "content", prompt);
                    cJSON_AddItemToArray(messages_with_plan, usr);
                    AIRY_FREE(plan_str);

                    run_rc = gw_run_tool_loop(ctx, model, prompt, messages_with_plan, active,
                                              &tool_trace, &final_text, &total_tokens, &total_cost);
                    cJSON_Delete(messages_with_plan);
                } else {
                    run_rc = gw_run_tool_loop(ctx, model, prompt, history, active, &tool_trace,
                                              &final_text, &total_tokens, &total_cost);
                }
            } else {
                run_rc = gw_run_tool_loop(ctx, model, prompt, history, active, &tool_trace,
                                          &final_text, &total_tokens, &total_cost);
            }
        } else {

            run_rc = gw_run_tool_loop(ctx, model, prompt, history, active, &tool_trace, &final_text,
                                      &total_tokens, &total_cost);
        }
    }
    gw_active_unregister(ctx, active);
    LOG_INFO("gateway: agent.run done (session=%s, rc=%d, tokens=%llu, cost=%.4f)", session_id,
             run_rc, (unsigned long long)total_tokens, total_cost);

    /* Persist the conversation to mem_d automatically at the end (M6 fix:
     * mem_d is no longer a dangling service). Only written on success (rc==0);
     * user cancellation/failure produces no partial memory. */
    if (run_rc == 0) {
        gw_persist_conversation(ctx, session_id, prompt, final_text ? final_text : "");
    }

    if (run_rc == 1) {

        cJSON *err_out = cJSON_CreateObject();
        cJSON_AddStringToObject(err_out, "jsonrpc", "2.0");
        if (id && cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(err_out, "id", id->valuedouble);
        } else {
            cJSON_AddNullToObject(err_out, "id");
        }
        cJSON *err = cJSON_CreateObject();
        cJSON_AddNumberToObject(err, "code", -32800);
        cJSON_AddStringToObject(err, "message", "Request cancelled by user");
        cJSON_AddStringToObject(err, "data", session_id);
        cJSON_AddItemToObject(err_out, "error", err);
        char *err_str = cJSON_PrintUnformatted(err_out);
        cJSON_Delete(err_out);
        if (tool_trace)
            cJSON_Delete(tool_trace);
        if (think_result)
            cJSON_Delete(think_result);
        if (final_text)
            AIRY_FREE(final_text);
        if (agent_spec_owned)
            cJSON_Delete(agent_spec_owned);
        return err_str;
    }
    if (run_rc != 0) {

        if (tool_trace)
            cJSON_Delete(tool_trace);
        if (think_result)
            cJSON_Delete(think_result);
        if (final_text)
            AIRY_FREE(final_text);
        if (agent_spec_owned)
            cJSON_Delete(agent_spec_owned);
        return jsonrpc_error(-32603, "agent.run failed: tool loop exhausted or LLM service error",
                             id);
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id && cJSON_IsNumber(id)) {
        cJSON_AddNumberToObject(out, "id", id->valuedouble);
    } else {
        cJSON_AddNullToObject(out, "id");
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "session_id", session_id);
    cJSON_AddStringToObject(result, "response", final_text ? final_text : "");
    cJSON_AddNumberToObject(result, "tokens_used", (double)total_tokens);
    cJSON_AddNumberToObject(result, "cost_usd", total_cost);
    if (tool_trace) {
        cJSON_AddItemToObject(result, "tool_trace", tool_trace);
    } else {
        cJSON_AddItemToObject(result, "tool_trace", cJSON_CreateArray());
    }
    /* Attach the dual-thinking result (GCCP+GRAD DAG plan + thinking events +
     * stats). NULL when think_d was unreachable; the field is omitted for
     * backward compatibility with old clients. */
    if (think_result) {
        cJSON_AddItemToObject(result, "thinking", think_result);
        think_result = NULL;
    }
    cJSON_AddItemToObject(out, "result", result);

    char *out_str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (final_text)
        AIRY_FREE(final_text);
    if (agent_spec_owned)
        cJSON_Delete(agent_spec_owned);
    return out_str;
}

/**
 * @brief agent.cancel: manually abort an in-flight agent.run request
 *
 * params.session_id -> look up the entry in the in-flight registry and set
 * cancelled. The tool loop checks this flag between rounds and stops, then
 * returns a -32800 error to the original request.
 *
 * @return JSON-RPC response (result.status="cancelling" on success; error if not found)
 */
static char *handle_agent_cancel(cJSON *root, gateway_business_ctx_t *ctx)
{
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *sid = params ? cJSON_GetObjectItem(params, "session_id") : NULL;
    if (!cJSON_IsString(sid) || !sid->valuestring || !*sid->valuestring) {
        return jsonrpc_error(-32602, "Invalid params: missing session_id", id);
    }

    airy_mtx_lock(&ctx->active_lock);
    gw_active_request_t *entry = ctx->active_requests;
    while (entry) {
        if (strcmp(entry->session_id, sid->valuestring) == 0)
            break;
        entry = entry->next;
    }
    if (entry) {
        atomic_store_explicit(&entry->cancelled, 1, memory_order_relaxed);
        LOG_INFO("gateway: agent.cancel set (session=%s)", sid->valuestring);
    }
    airy_mtx_unlock(&ctx->active_lock);

    if (!entry) {
        LOG_DEBUG("gateway: agent.cancel miss (session=%s, 请求已完成或不存在)", sid->valuestring);
        return jsonrpc_error(-32004, "No active request with given session_id", id);
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "jsonrpc", "2.0");
    if (id && cJSON_IsNumber(id)) {
        cJSON_AddNumberToObject(out, "id", id->valuedouble);
    } else {
        cJSON_AddNullToObject(out, "id");
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "cancelling");
    cJSON_AddStringToObject(result, "session_id", sid->valuestring);
    cJSON_AddItemToObject(out, "result", result);
    char *out_str = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return out_str;
}

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
     * $AIRY_HOME/run/<name>.sock
     */
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
        static const gw_ns_forward_rule_t rule = {"llm.", NULL, GW_LLM_METHODS,
                                                  GW_LLM_DEFAULT_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
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
        static const gw_ns_forward_rule_t rule = {"agent.", NULL, GW_AGENT_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->agent_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "tool.", 5) == 0) {
        /* tool.pending / tool.approve use the dedicated branches above
         * (approval flow); other tool.* methods (list/execute/health_check)
         * forward generically */
        static const gw_ns_forward_rule_t rule = {"tool.", NULL, GW_TOOL_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->tool_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "a2a.", 4) == 0) {
        static const gw_ns_forward_rule_t rule = {"a2a.", NULL, GW_A2A_METHODS,
                                                  GW_LLM_DEFAULT_TIMEOUT_MS};

        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->a2a_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "plugin.", 7) == 0) {
        static const gw_ns_forward_rule_t rule = {"plugin.", NULL, GW_PLUGIN_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->plugin_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "info.", 5) == 0) {
        static const gw_ns_forward_rule_t rule = {"info.", NULL, GW_INFO_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->info_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "notify.", 7) == 0) {
        static const gw_ns_forward_rule_t rule = {"notify.", NULL, GW_NOTIFY_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->notify_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "observe.", 8) == 0) {
        static const gw_ns_forward_rule_t rule = {"observe.", NULL, GW_OBSERVE_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->observe_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "market.", 7) == 0) {
        static const gw_ns_forward_rule_t rule = {"market.", NULL, GW_MARKET_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->market_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "hook.", 5) == 0) {
        static const gw_ns_forward_rule_t rule = {"hook.", NULL, GW_HOOK_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->hook_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "sched.", 6) == 0) {
        static const gw_ns_forward_rule_t rule = {"sched.", NULL, GW_SCHED_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->sched_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "think.", 6) == 0) {
        static const gw_ns_forward_rule_t rule = {"think.", NULL, GW_THINK_METHODS,
                                                  GW_THINK_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->think_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "monit.", 6) == 0) {
        static const gw_ns_forward_rule_t rule = {"monit.", NULL, GW_MONIT_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->monit_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "channel.", 8) == 0) {
        static const gw_ns_forward_rule_t rule = {"channel.", NULL, GW_CHANNEL_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
        r2.sock_path = ctx->channel_sock_path;
        resp = handle_ns_forward(root, &r2);
    } else if (strncmp(method->valuestring, "cupolas.", 8) == 0) {
        static const gw_ns_forward_rule_t rule = {"cupolas.", NULL, GW_CUPOLAS_METHODS,
                                                  GW_TOOL_TIMEOUT_MS};
        gw_ns_forward_rule_t r2 = rule;
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

/**
 * @brief MCP tool execution backend: tools/call -> tool_d.execute_tool
 *
 * The returned result_json is a valid JSON string (quoted, so MCP tools/call's
 * content[].text can embed it directly via %s); its content is the tool_d
 * execution output or an error description.
 */
int gw_biz_tool_exec(const char *tool_name, const char *arguments_json, char **result_json,
                     void *user_data)
{
    const gateway_business_ctx_t *ctx = (const gateway_business_ctx_t *)user_data;
    *result_json = NULL;
    if (!ctx || !tool_name) {
        *result_json = AIRY_STRDUP("\"Invalid tool request\"");
        return -1;
    }

    if (gw_acl_check_tool(tool_name) != 0) {
        *result_json = AIRY_STRDUP("\"Permission denied: tool not authorized\"");
        return -1;
    }

    cJSON *params = cJSON_CreateObject();
    if (!params) {
        *result_json = AIRY_STRDUP("\"Out of memory\"");
        return -1;
    }
    cJSON_AddStringToObject(params, "tool_id", tool_name);
    cJSON *pargs = cJSON_Parse(arguments_json && arguments_json[0] ? arguments_json : "{}");
    cJSON_AddItemToObject(params, "params", pargs ? pargs : cJSON_CreateObject());
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str) {
        *result_json = AIRY_STRDUP("\"Out of memory\"");
        return -1;
    }

    char *resp = gw_svc_call(ctx->tool_sock_path, "execute_tool", params_str, GW_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp) {
        *result_json = AIRY_STRDUP("\"Tool service unreachable\"");
        return -1;
    }

    char *text = NULL;
    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root) {
        *result_json = AIRY_STRDUP("\"Tool service returned invalid response\"");
        return -1;
    }
    cJSON *err = cJSON_GetObjectItem(root, "error");
    cJSON *result = err ? NULL : cJSON_GetObjectItem(root, "result");
    if (result) {
        cJSON *success = cJSON_GetObjectItem(result, "success");
        cJSON *output = cJSON_GetObjectItem(result, "output");
        cJSON *error = cJSON_GetObjectItem(result, "error");
        if (cJSON_IsNumber(success) && success->valueint != 0 && cJSON_IsString(output)) {
            text = AIRY_STRDUP(output->valuestring);
        } else if (cJSON_IsString(error) && error->valuestring) {
            size_t n = strlen(error->valuestring) + 16;
            text = (char *)AIRY_MALLOC(n);
            if (text)
                snprintf(text, n, "Error: %s", error->valuestring);
        }
    } else if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            size_t n = strlen(msg->valuestring) + 16;
            text = (char *)AIRY_MALLOC(n);
            if (text)
                snprintf(text, n, "Error: %s", msg->valuestring);
        }
    }
    cJSON_Delete(root);
    if (!text)
        text = AIRY_STRDUP("(no output)");

    cJSON *jstr = cJSON_CreateString(text);
    AIRY_FREE(text);
    *result_json = jstr ? cJSON_PrintUnformatted(jstr) : AIRY_STRDUP("\"\"");
    if (jstr)
        cJSON_Delete(jstr);
    return 0;
}

/**
 * @brief OpenAI LLM backend: chat/completions -> llm_d.complete
 *
 * Calls llm_d and converts the response into the OpenAI chat.completion format
 * (choices[0].message.content / tool_calls) so clients never see the internal
 * JSON-RPC.
 */
int gw_biz_llm_complete(const char *model, const char *messages_json, const char *functions_json,
                        double temperature, int max_tokens, char **response_json, void *user_data)
{
    const gateway_business_ctx_t *ctx = (const gateway_business_ctx_t *)user_data;
    *response_json = NULL;
    if (!ctx) {
        return -1;
    }

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON_AddStringToObject(params, "model", (model && model[0]) ? model : ctx->default_model);
    cJSON *msgs = cJSON_Parse(messages_json && messages_json[0] ? messages_json : "[]");
    cJSON_AddItemToObject(params, "messages", msgs ? msgs : cJSON_CreateArray());

    if (functions_json && functions_json[0]) {
        cJSON *tools = cJSON_Parse(functions_json);
        if (tools) {
            cJSON_AddItemToObject(params, "tools", tools);
        } else {
            cJSON_AddItemToObject(params, "tools", cJSON_CreateArray());
        }
    }
    cJSON_AddNumberToObject(params, "max_tokens", max_tokens > 0 ? max_tokens : 2048);
    cJSON_AddNumberToObject(params, "temperature", temperature);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    char *resp = gw_svc_call(ctx->llm_sock_path, "complete", params_str, GW_LLM_DEFAULT_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp)
        return -1;

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root)
        return -1;

    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {

        *response_json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return 0;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *choices = result ? cJSON_GetObjectItem(result, "choices") : NULL;
    cJSON *choice0 =
        (choices && cJSON_GetArraySize(choices) > 0) ? cJSON_GetArrayItem(choices, 0) : NULL;

    cJSON *openai = cJSON_CreateObject();
    char idbuf[64];
    snprintf(idbuf, sizeof(idbuf), "chatcmpl-%ld", (long)time(NULL));
    cJSON_AddStringToObject(openai, "id", idbuf);
    cJSON_AddStringToObject(openai, "object", "chat.completion");
    cJSON_AddNumberToObject(openai, "created", (double)time(NULL));
    cJSON_AddStringToObject(openai, "model", (model && model[0]) ? model : ctx->default_model);
    cJSON *choices_out = cJSON_CreateArray();
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddNumberToObject(choice, "index", 0);
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "assistant");
    if (choice0) {
        cJSON *content = cJSON_GetObjectItem(choice0, "content");
        cJSON_AddStringToObject(message, "content",
                                cJSON_IsString(content) ? content->valuestring : "");
        cJSON *tool_calls = cJSON_GetObjectItem(choice0, "tool_calls");
        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
            cJSON_AddItemToObject(message, "tool_calls", cJSON_Duplicate(tool_calls, 1));
        }
        cJSON *reason = cJSON_GetObjectItem(choice0, "finish_reason");
        cJSON_AddStringToObject(choice, "finish_reason",
                                cJSON_IsString(reason) ? reason->valuestring : "stop");
    } else {
        cJSON_AddStringToObject(message, "content", "");
        cJSON_AddStringToObject(choice, "finish_reason", "stop");
    }
    cJSON_AddItemToObject(choice, "message", message);
    cJSON_AddItemToArray(choices_out, choice);
    cJSON_AddItemToObject(openai, "choices", choices_out);
    if (result) {
        cJSON *usage = cJSON_GetObjectItem(result, "usage");
        if (cJSON_IsObject(usage)) {
            cJSON_AddItemToObject(openai, "usage", cJSON_Duplicate(usage, 1));
        }
    }

    *response_json = cJSON_PrintUnformatted(openai);
    cJSON_Delete(openai);
    cJSON_Delete(root);
    return *response_json ? 0 : -1;
}

/**
 * @brief A2A task backend: task -> sched_d.schedule_task
 *
 * The output is the scheduling-result JSON
 * (selected_agent_id/confidence/estimated_time_ms), embedded directly into the
 * A2A task response's output field.
 */
int gw_biz_sched_schedule(const char *task_id, const char *task_type, const char *input_json,
                          char **output_json, void *user_data)
{
    const gateway_business_ctx_t *ctx = (const gateway_business_ctx_t *)user_data;
    *output_json = NULL;
    if (!ctx || !task_id) {
        return -1;
    }

    cJSON *params = cJSON_CreateObject();
    if (!params)
        return -1;
    cJSON *task = cJSON_CreateObject();
    cJSON_AddStringToObject(task, "task_id", task_id);
    char desc[512];
    snprintf(desc, sizeof(desc), "A2A delegated task (type=%s)", task_type ? task_type : "unknown");
    cJSON_AddStringToObject(task, "task_description", desc);
    cJSON_AddNumberToObject(task, "priority", 0);
    cJSON_AddNumberToObject(task, "timeout_ms", 30000);

    if (input_json && input_json[0]) {
        cJSON *input = cJSON_Parse(input_json);
        if (input) {
            cJSON_AddItemToObject(task, "input", input);
        }
    }
    cJSON_AddItemToObject(params, "task", task);
    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!params_str)
        return -1;

    char *resp = gw_svc_call(ctx->sched_sock_path, "schedule_task", params_str, GW_TOOL_TIMEOUT_MS);
    AIRY_FREE(params_str);
    if (!resp)
        return -1;

    cJSON *root = cJSON_Parse(resp);
    AIRY_FREE(resp);
    if (!root)
        return -1;

    int rc = -1;
    cJSON *err = cJSON_GetObjectItem(root, "error");
    cJSON *result = err ? NULL : cJSON_GetObjectItem(root, "result");
    if (result) {
        *output_json = cJSON_PrintUnformatted(result);
        rc = *output_json ? 0 : -1;
    } else {
        cJSON *msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
        const char *m =
            (err && cJSON_IsString(msg) && msg->valuestring) ? msg->valuestring : "schedule failed";
        size_t n = strlen(m) + 32;
        char *ebuf = (char *)AIRY_MALLOC(n);
        if (ebuf) {
            snprintf(ebuf, n, "{\"error\":\"%s\"}", m);
            *output_json = ebuf;
        }
    }
    cJSON_Delete(root);
    return rc;
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
