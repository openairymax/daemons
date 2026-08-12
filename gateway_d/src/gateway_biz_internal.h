/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @owner: team-B */
/**
 * @file gateway_biz_internal.h
 * @brief 网关业务处理器内部共享定义（模块内私有，勿对外导出）
 *
 * 将 2608 行的 gateway_business_handler.c 按单一职责拆分为四个文件，
 * 本头承载它们之间的共享契约：
 *   - gateway_biz_forward.c  命名空间转发（L2 协议客户端 + 白名单）
 *   - gateway_biz_agent.c    agent.run 编排（双思考 + 工具循环 + 取消）
 *   - gateway_biz_backend.c  MCP/OpenAI/A2A 协议后端
 *   - gateway_business_handler.c  主分发与 ctx 生命周期
 */

#ifndef AIRY_RT_DAEMON_GATEWAY_BIZ_INTERNAL_H
#define AIRY_RT_DAEMON_GATEWAY_BIZ_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <cjson/cJSON.h>

#include "gateway_business_handler.h"

#include "airy_memory.h"
#include "atomic_compat.h"

#ifdef __cplusplus
extern "C" {
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

#define GW_MAX_TOOL_LOOPS 8
/* Tool execution timeout 90s: shell_run itself times out at 60s; the old 30s
 * value would hit recv timeout before the tool finished, making the gateway
 * wrongly report long commands as failed. */
#define GW_TOOL_TIMEOUT_MS 90000

#define GW_AGENT_SPAWN_TIMEOUT_MS 90000
#define GW_AGENT_INVOKE_TIMEOUT_MS 180000

#define GW_AGENT_FILE_MAX (64 * 1024)

/* @brief In-flight agent.run request entry
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

/* @brief Gateway business context: resolved daemon socket paths + model config
 *
 * All daemon endpoints are resolved once at create time
 * (env override -> $AIRY_RUNTIME_DIR/<name>.sock -> <name>.sock). */
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

/* @brief Namespace forwarding rule: <ns>.<method> -> target daemon <method>
 *        (whitelist)
 *
 * Only L2 methods registered by each daemon are allowed
 * (02-l2-service-protocol.md §6), preventing arbitrary method pass-through.
 * Sensitive methods such as execute must be listed explicitly by the caller. */
typedef struct {
    const char *ns;
    const char *sock_path;
    const char *const *methods;
    int timeout_ms;
} gw_ns_forward_rule_t;

/* ---- gateway_biz_forward.c（L2 协议客户端 + 白名单转发） ---- */
char *jsonrpc_error(int code, const char *msg, const cJSON *id);
char *gw_svc_call(const char *sock_path, const char *method, const char *params_json,
                  int timeout_ms);
int gw_think_process(const gateway_business_ctx_t *ctx, const char *prompt, cJSON **out_think);
int gw_acl_check_tool(const char *tool_name);
const char *gw_mem_method_allowlist(const char *method);
char *handle_ns_forward(cJSON *root, const gw_ns_forward_rule_t *rule);
char *handle_mem_call(cJSON *root, const gateway_business_ctx_t *ctx);
char *handle_llm_list_models(cJSON *root, const gateway_business_ctx_t *ctx);
char *handle_tool_approval_call(cJSON *root, const gateway_business_ctx_t *ctx,
                                const char *tool_method);

/* Namespace forwarding rules (const, defined in gateway_biz_forward.c) */
extern const gw_ns_forward_rule_t GW_NS_LLM;
extern const gw_ns_forward_rule_t GW_NS_AGENT;
extern const gw_ns_forward_rule_t GW_NS_TOOL;
extern const gw_ns_forward_rule_t GW_NS_A2A;
extern const gw_ns_forward_rule_t GW_NS_PLUGIN;
extern const gw_ns_forward_rule_t GW_NS_INFO;
extern const gw_ns_forward_rule_t GW_NS_NOTIFY;
extern const gw_ns_forward_rule_t GW_NS_OBSERVE;
extern const gw_ns_forward_rule_t GW_NS_MARKET;
extern const gw_ns_forward_rule_t GW_NS_HOOK;
extern const gw_ns_forward_rule_t GW_NS_SCHED;
extern const gw_ns_forward_rule_t GW_NS_THINK;
extern const gw_ns_forward_rule_t GW_NS_MONIT;
extern const gw_ns_forward_rule_t GW_NS_CHANNEL;
extern const gw_ns_forward_rule_t GW_NS_CUPOLAS;

/* ---- gateway_biz_llm.c（LLM 调用 + 工具循环） ---- */
int gw_run_tool_loop(const gateway_business_ctx_t *ctx, const char *model, const char *prompt,
                     const cJSON *history, gw_active_request_t *active, cJSON **out_trace,
                     char **out_text, uint64_t *out_tokens, double *out_cost);

/* ---- gateway_biz_agent.c（agent.run 编排） ---- */
bool gw_active_is_cancelled(gw_active_request_t *entry);
char *handle_agent_run(cJSON *root, gateway_business_ctx_t *ctx);
char *handle_agent_cancel(cJSON *root, gateway_business_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_GATEWAY_BIZ_INTERNAL_H */
