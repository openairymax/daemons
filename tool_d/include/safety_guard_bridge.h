/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file safety_guard_bridge.h
 * @brief C-L05: Cupolas SafetyGuard → tool_d 桥接层
 *
 * 将 Cupolas 安全穹顶的 safety_guard_check_chain() API
 * 桥接到 tool_d 的工具审批流程中，实现 6 种守卫类型的
 * 权限检查、速率限制、内容过滤等安全控制。
 *
 * 守卫类型映射：
 *   SAFETY_GUARD_PERMISSION   → RBAC 权限检查
 *   SAFETY_GUARD_RATE_LIMIT   → 工具调用频率限制
 *   SAFETY_GUARD_CONTENT_FILTER → 输入内容过滤
 *   SAFETY_GUARD_INPUT        → 参数净化
 *   SAFETY_GUARD_RESOURCE     → 资源配额检查
 *   SAFETY_GUARD_AUDIT        → 审计日志记录
 */

#ifndef AIRY_RT_SAFETY_GUARD_BRIDGE_H
#define AIRY_RT_SAFETY_GUARD_BRIDGE_H

#include "tool_approval.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct safety_guard_bridge_s safety_guard_bridge_t;


typedef struct {
    bool enable_permission_guard;
    bool enable_rate_limit_guard;
    bool enable_content_filter;
    bool enable_input_sanitization;
    bool enable_resource_quota;
    bool enable_audit_guard;
    uint32_t rate_limit_per_minute;
    uint32_t max_params_size;
    const char *denied_patterns;
    const char *agent_id; /**< Agent ID */
} safety_guard_bridge_config_t;


typedef struct {
    int permission_passed;
    int rate_limit_passed;
    int content_filter_passed;
    int input_sanitized;
    int resource_quota_passed;
    int audit_recorded;
    char denial_reason[256];
    char sanitized_params[4096];
    int guard_chain_length;
    int guards_executed;
} safety_guard_bridge_result_t;


/**
 * @brief 创建 SafetyGuard 桥接层
 * @param config 桥接配置（NULL 使用默认：所有守卫启用）
 * @return 桥接句柄，失败返回 NULL
 * @ownership return: OWNER
 */
safety_guard_bridge_t *safety_guard_bridge_create(const safety_guard_bridge_config_t *config);

/**
 * @brief 销毁 SafetyGuard 桥接层
 * @param bridge 桥接句柄
 * @ownership bridge: TRANSFER
 */
void safety_guard_bridge_destroy(safety_guard_bridge_t *bridge);


/**
 * @brief C-L05: 执行完整的 SafetyGuard 守卫链检查
 *
 * 依次执行 6 种守卫类型：
 *   1. SAFETY_GUARD_PERMISSION   → RBAC 权限检查
 *   2. SAFETY_GUARD_RATE_LIMIT   → 频率限制
 *   3. SAFETY_GUARD_CONTENT_FILTER → 内容过滤
 *   4. SAFETY_GUARD_INPUT        → 参数净化
 *   5. SAFETY_GUARD_RESOURCE     → 资源配额
 *   6. SAFETY_GUARD_AUDIT        → 审计日志
 *
 * 任一守卫返回 DENY → 立即终止并返回拒绝
 *
 * @param bridge 桥接句柄
 * @param meta 工具元数据
 * @param params_json 原始参数 JSON
 * @param result 输出检查结果
 * @return 0 全部通过，非0 被拒绝
 * @ownership bridge: BORROW, meta: BORROW, params_json: BORROW, result: BORROW
 */
int safety_guard_bridge_check(safety_guard_bridge_t *bridge, const tool_metadata_t *meta,
                              const char *params_json, safety_guard_bridge_result_t *result);

/**
 * @brief 以指定 agent 身份执行完整 SafetyGuard 守卫链检查
 *
 * 与 safety_guard_bridge_check 等价，但权限/审计守卫使用传入的 agent_id
 * 而非桥接层默认 agent_id。用于按请求透传真实 Agent 身份（如 agent_d 子进程
 * 的 coding_v1），使 ACL 按真实主体判定，未授权工具进入交互式审批。
 *
 * @param bridge 桥接句柄
 * @param agent_id 本次请求的 Agent ID（NULL 时回退桥接层默认）
 * @param meta 工具元数据
 * @param params_json 原始参数 JSON
 * @param result 输出检查结果
 * @return 0 全部通过，非0 被拒绝
 */
int safety_guard_bridge_check_for_agent(safety_guard_bridge_t *bridge, const char *agent_id,
                                        const tool_metadata_t *meta, const char *params_json,
                                        safety_guard_bridge_result_t *result);

/**
 * @brief 仅执行权限守卫检查
 * @param bridge 桥接句柄
 * @param agent_id Agent ID
 * @param tool_name 工具名称
 * @param action 操作（"execute"/"register"/"list"）
 * @return 0 通过，非0 拒绝
 */
int safety_guard_bridge_check_permission(safety_guard_bridge_t *bridge, const char *agent_id,
                                         const char *tool_name, const char *action);

/**
 * @brief 仅执行速率限制检查
 * @param bridge 桥接句柄
 * @param tool_name 工具名称
 * @return 0 通过，非0 超出限制
 */
int safety_guard_bridge_check_rate_limit(safety_guard_bridge_t *bridge, const char *tool_name);

/**
 * @brief 仅执行内容过滤检查
 * @param bridge 桥接句柄
 * @param params_json 参数 JSON
 * @param sanitized_params 输出净化后参数
 * @param sanitized_size 输出缓冲区大小
 * @return 0 通过，非0 被过滤
 */
int safety_guard_bridge_filter_content(safety_guard_bridge_t *bridge, const char *params_json,
                                       char *sanitized_params, size_t sanitized_size);


/**
 * @brief 记录审计日志事件
 * @param bridge 桥接句柄
 * @param event_type 事件类型
 * @param tool_name 工具名称
 * @param decision 决策结果
 * @param reason 原因
 * @param agent_id Agent ID
 * @return 0 成功
 */
int safety_guard_bridge_audit_log(safety_guard_bridge_t *bridge, const char *event_type,
                                  const char *tool_name, int decision, const char *reason,
                                  const char *agent_id);

/**
 * @brief 获取桥接层统计信息
 * @param bridge 桥接句柄
 * @param out_total_checks 输出总检查次数
 * @param out_denied_count 输出拒绝次数
 * @param out_rate_limited 输出速率限制次数
 */
void safety_guard_bridge_get_stats(safety_guard_bridge_t *bridge, uint64_t *out_total_checks,
                                   uint64_t *out_denied_count, uint64_t *out_rate_limited);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_SAFETY_GUARD_BRIDGE_H */