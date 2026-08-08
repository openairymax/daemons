// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file tool_interactive_approval.h
 * @brief P0：工具级交互式权限审批（Claude Code 风格 permission prompt）
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 *
 * 当 AIRY_TOOL_APPROVAL_MODE=interactive 时，被静态审批拒绝的工具执行不再
 * fail-closed 直接返回 EPERM，而是入队一个 pending 审批请求并阻塞等待外部
 * tool.approve 决议（allow/always/deny），超时默认 AIRY_TOOL_APPROVAL_TIMEOUT_MS
 * (120000ms)。
 */

#ifndef AIRY_RT_TOOL_INTERACTIVE_APPROVAL_H
#define AIRY_RT_TOOL_INTERACTIVE_APPROVAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 交互审批决议结果
 */
typedef enum {
    AIRY_APPROVAL_DENIED = 0, /**< deny 决议或等待超时 */
    AIRY_APPROVAL_ALLOWED,    /**< allow 决议：放行本次执行 */
    AIRY_APPROVAL_ALWAYS      /**< always 决议：放行并加入持久 ACL */
} airy_approval_outcome_t;

/* 交互审批管理器（不透明类型） */
typedef struct interactive_approval interactive_approval_t;

/**
 * @brief 创建交互审批管理器（读取环境变量决定是否启用）
 * @return 管理器句柄，失败返回 NULL
 *
 * 环境变量：
 * - AIRY_TOOL_APPROVAL_MODE：为 "interactive" 时启用交互审批（默认 static）
 * - AIRY_TOOL_APPROVAL_TIMEOUT_MS：阻塞等待超时（默认 120000ms）
 *
 * @ownership return: OWNER
 */
interactive_approval_t *interactive_approval_create(void);

/**
 * @brief 销毁交互审批管理器
 * @param mgr 管理器
 *
 * @ownership mgr: TRANSFER
 */
void interactive_approval_destroy(interactive_approval_t *mgr);

/**
 * @brief 交互审批是否启用（AIRY_TOOL_APPROVAL_MODE=interactive）
 * @param mgr 管理器
 * @return true 启用，false 未启用
 *
 * @ownership mgr: BORROW
 */
bool interactive_approval_is_enabled(const interactive_approval_t *mgr);

/**
 * @brief 入队 pending 审批请求并阻塞等待外部决议
 *
 * 调用线程阻塞直到 tool.approve 决议或超时。超时按 AIRY_TOOL_APPROVAL_TIMEOUT_MS
 * 计算，超时决议为 AIRY_APPROVAL_DENIED。
 *
 * @param mgr 管理器
 * @param tool 工具名称
 * @param agent_id 调用者 Agent ID
 * @param params_json 工具参数 JSON（副本保存）
 * @param out_outcome 输出决议结果（allow/always/deny/超时）
 * @return 请求 ID 字符串（AIRY_MALLOC，调用者 AIRY_FREE），失败返回 NULL
 *
 * @ownership params_json: BORROW; return: OWNER
 */
char *interactive_approval_block(interactive_approval_t *mgr, const char *tool,
                                 const char *agent_id, const char *params_json,
                                 airy_approval_outcome_t *out_outcome);

/**
 * @brief 按 request_id 决议一个 pending 请求
 * @param mgr 管理器
 * @param request_id 请求 ID
 * @param decision 决议："allow" / "always" / "deny"
 * @return 0 成功；未找到请求 AIRY_ERR_NOT_FOUND；参数非法 AIRY_ERR_INVALID_PARAM
 *
 * @ownership mgr: BORROW
 */
int interactive_approval_resolve(interactive_approval_t *mgr, const char *request_id,
                                 const char *decision);

/**
 * @brief 列出所有 pending 审批请求（JSON 数组字符串）
 * @param mgr 管理器
 * @return JSON 数组字符串（AIRY_MALLOC，调用者 AIRY_FREE），失败返回 NULL
 *
 * 每个元素: {request_id, tool, agent_id, params, created_at}
 *
 * @ownership return: OWNER
 */
char *interactive_approval_pending_list_json(interactive_approval_t *mgr);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_TOOL_INTERACTIVE_APPROVAL_H */