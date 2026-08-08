// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file executor.h
 * @brief 工具执行器接口
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef TOOL_EXECUTOR_H
#define TOOL_EXECUTOR_H

#include "config.h"
#include "tool_approval.h"
#include "tool_interactive_approval.h"
#include "tool_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool_executor tool_executor_t;

typedef struct {
    int max_workers;
    int timeout_sec;
    char *workbench_type;
} tool_executor_config_t;

tool_executor_t *tool_executor_create(const tool_executor_config_t *cfg);
tool_executor_t *tool_executor_create_ex(const tool_executor_config_t *ecfg);
void tool_executor_destroy(tool_executor_t *exec);

/**
 * @brief 执行工具
 * @param exec 执行器
 * @param meta 工具元数据
 * @param params_json 参数 JSON
 * @param agent_id 调用方 Agent ID（NULL/空串回退审批上下文默认，"tool_d"）
 * @param out_result 输出结果
 * @return 0 成功，其他错误码
 */
int tool_executor_run(tool_executor_t *exec, const tool_metadata_t *meta, const char *params_json,
                      const char *agent_id, tool_result_t **out_result);

typedef void (*tool_execute_callback_t)(tool_result_t *result, void *user_data);
int tool_executor_run_async(tool_executor_t *exec, const tool_metadata_t *meta,
                            const char *params_json, const char *agent_id,
                            tool_execute_callback_t callback, void *user_data,
                            tool_result_t **out_result);

/* C-L05: 设置工具审批上下文（Cupolas SafetyGuard → tool_d） */
void tool_executor_set_approval_ctx(tool_executor_t *exec, tool_approval_ctx_t *approval_ctx);

/* P0 交互式审批：暴露给 service 层的接口 */

/**
 * @brief 交互式审批是否启用
 * @param exec 执行器
 * @return true 启用，false 未启用
 *
 * @ownership exec: BORROW
 */
bool tool_executor_interactive_enabled(tool_executor_t *exec);

/**
 * @brief 列出所有 pending 审批请求（JSON 数组字符串）
 * @param exec 执行器
 * @return JSON 数组字符串（AIRY_MALLOC，调用者 AIRY_FREE），失败返回 NULL
 *
 * @ownership exec: BORROW; return: OWNER
 */
char *tool_executor_interactive_pending_list(tool_executor_t *exec);

/**
 * @brief 按 request_id 决议一个 pending 审批请求
 * @param exec 执行器
 * @param request_id 请求 ID
 * @param decision 决议："allow" / "always" / "deny"
 * @return 0 成功；未找到 AIRY_ERR_NOT_FOUND；参数非法 AIRY_ERR_INVALID_PARAM
 *
 * @ownership exec: BORROW
 */
int tool_executor_interactive_resolve(tool_executor_t *exec, const char *request_id,
                                      const char *decision);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_EXECUTOR_H */