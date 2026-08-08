// SPDX-FileCopyrightText: 2026 SPHARX.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
// @owner: team-B
/**
 * @file gateway_business_handler.h
 * @brief 网关业务请求处理器（agent.run → llm_d 转发）
 *
 * 提供 HTTP 网关的默认业务处理链：
 *   标准 JSON-RPC agent.run → llm_d(complete) → JSON-RPC result
 * 修复此前 HTTP 网关 handler 从未接线（恒返回 "Custom handler failed"）的缺口。
 */

#ifndef AIRY_RT_DAEMON_GATEWAY_BUSINESS_HANDLER_H
#define AIRY_RT_DAEMON_GATEWAY_BUSINESS_HANDLER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前向声明：协议路由器（见 protocol/gateway_protocol_router.h） */
typedef struct gw_proto_router gw_proto_router_t;

/**
 * @brief 业务处理器上下文
 */
typedef struct gateway_business_ctx_s gateway_business_ctx_t;

/**
 * @brief 统一协议入口上下文：协议路由 + JSON-RPC 业务处理
 */
typedef struct {
    gateway_business_ctx_t *biz_ctx; /**< 业务处理器上下文（agent.run 等 JSON-RPC 业务） */
    gw_proto_router_t *router;       /**< 协议路由器（MCP/OpenAI/A2A 适配器） */
} gateway_entry_ctx_t;

/**
 * @brief 创建业务处理器上下文
 *
 * 从环境变量解析 llm_d 端点：
 *   - AIRY_LLM_SOCK：POSIX Unix socket 路径（默认 $AIRY_RUNTIME_DIR/llm.sock）
 *   - AIRY_LLM_TCP_ADDR / AIRY_LLM_TCP_PORT：Windows TCP 端点（默认 127.0.0.1:8080）
 *
 * @return 上下文指针，失败返回 NULL
 */
gateway_business_ctx_t *gateway_business_ctx_create(void);

/**
 * @brief 销毁业务处理器上下文
 * @param ctx 上下文指针
 */
void gateway_business_ctx_destroy(gateway_business_ctx_t *ctx);

/**
 * @brief L2 标准方法 <ns>.shutdown 回调类型（02-l2-service-protocol.md §6.1）
 *
 * gateway_business_handle 收到 "shutdown" 方法时调用，由宿主（gateway_d main）
 * 负责触发真实优雅退出（如原子置位 g_running 让主循环退出）。
 */
typedef void (*gateway_shutdown_fn_t)(void *user_data);

/**
 * @brief 设置 shutdown 回调（L2 <ns>.shutdown 支持）
 * @param ctx 业务处理器上下文
 * @param cb  shutdown 回调（可传 NULL 表示不支持）
 * @param user_data 回调用户数据（如 &g_running）
 * @return 0 成功，非 0 失败
 */
int gateway_business_ctx_set_shutdown_cb(gateway_business_ctx_t *ctx,
                                         gateway_shutdown_fn_t cb, void *user_data);

/**
 * @brief 网关业务请求处理器（gateway_service_handler_t 签名）
 *
 * 输入标准 JSON-RPC 请求字符串，支持：
 *   - "ping"      → {"result":{"status":"ok"}}
 *   - "agent.run" → 转发 llm_d.complete，返回对话结果
 *   - 其他        → -32601 Method not found
 *
 * @param request JSON-RPC 请求字符串
 * @param user_data gateway_business_ctx_t*
 * @return JSON 响应字符串（AIRY_MALLOC 分配），失败返回 NULL
 */
char *gateway_business_handle(void *request, void *user_data);

/* ==================== Phase 2: 协议适配 backend（内部服务调用） ==================== */

/**
 * @brief 统一协议入口（替换 gateway_business_handle 作为 HTTP 唯一 handler）
 *
 * 按 body 检测协议并路由：
 *   - MCP / OpenAI / A2A → 对应适配器（内部服务调用见下）
 *   - 其余 JSON-RPC（agent.run/ping）→ gateway_business_handle
 *
 * @param request JSON-RPC 请求字符串
 * @param user_data gateway_entry_ctx_t*
 * @return JSON 响应字符串（AIRY_MALLOC 分配），失败返回 NULL
 */
char *gateway_protocol_entry(void *request, void *user_data);

/**
 * @brief MCP 工具执行 backend：tools/call → tool_d.execute_tool
 * @param tool_name 工具名（fs_read/fs_write/fs_list/shell_run）
 * @param arguments_json 工具参数 JSON 字符串
 * @param result_json 输出结果（合法 JSON 字符串，AIRY_MALLOC，调用者 AIRY_FREE）
 * @param user_data gateway_business_ctx_t*
 * @return 0 成功，非 0 失败
 */
int gw_biz_tool_exec(const char *tool_name, const char *arguments_json,
                     char **result_json, void *user_data);

/**
 * @brief OpenAI LLM backend：chat/completions → llm_d.complete
 * @param model 模型名
 * @param messages_json 对话消息数组 JSON
 * @param functions_json OpenAI tools/functions 数组 JSON（可为 NULL）
 * @param temperature 采样温度
 * @param max_tokens 最大 token 数
 * @param response_json OpenAI chat.completion 格式响应（AIRY_MALLOC，调用者 AIRY_FREE）
 * @param user_data gateway_business_ctx_t*
 * @return 0 成功，非 0 失败
 */
int gw_biz_llm_complete(const char *model, const char *messages_json,
                        const char *functions_json, double temperature, int max_tokens,
                        char **response_json, void *user_data);

/**
 * @brief A2A 任务 backend：task → sched_d.schedule_task
 * @param task_id 任务 ID
 * @param task_type 任务类型（编码/分析等）
 * @param input_json 任务输入 JSON 字符串
 * @param output_json 调度结果（合法 JSON，AIRY_MALLOC，调用者 AIRY_FREE）
 * @param user_data gateway_business_ctx_t*
 * @return 0 成功，非 0 失败
 */
int gw_biz_sched_schedule(const char *task_id, const char *task_type,
                          const char *input_json, char **output_json, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_GATEWAY_BUSINESS_HANDLER_H */
