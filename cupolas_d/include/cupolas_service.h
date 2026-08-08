// SPDX-FileCopyrightText: 2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0
/**
 * @file cupolas_service.h
 * @brief Cupolas 安全穹顶服务对外接口（cupolas.* 命名空间）
 *
 * cupolas_d 将 cupolas 安全库（权限引擎/输入净化/审计/工作台）封装为独立
 * daemon 服务，通过 Unix socket JSON-RPC 暴露 cupolas.* 命名空间方法。
 *
 * 暴露方法：
 *   - cupolas.check_permission : 权限裁决
 *   - cupolas.sanitize         : 输入净化
 *   - cupolas.execute_command  : 隔离工位命令执行
 *   - cupolas.add_rule         : 动态添加权限规则
 *   - cupolas.audit_flush      : 刷新审计日志
 *   - cupolas.get_stats        : 服务统计（真实计数）
 *   - cupolas.shutdown         : 优雅退出（由 main.c 宏生成）
 *
 * 本接口定义服务生命周期、方法参数结构与结果结构。服务实现位于
 * src/service.c，所有方法真实调用 agentrt/cupolas/include/cupolas.h API。
 *
 * @copyright (c) 2026 SPHARX. All Rights Reserved.
 */

#ifndef AIRY_RT_DAEMON_CUPOLAS_SERVICE_H
#define AIRY_RT_DAEMON_CUPOLAS_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cupolas_service cupolas_service_t;

/* ==================== 方法参数结构体 ==================== */

/** @brief cupolas.check_permission 参数 */
typedef struct {
    const char *agent_id;   /**< 主体标识（必填） */
    const char *action;     /**< 动作：read/write/execute（必填） */
    const char *resource;   /**< 资源路径（必填） */
    const char *context;    /**< 上下文信息（可 NULL） */
} cupolas_check_permission_params_t;

/** @brief cupolas.sanitize 参数 */
typedef struct {
    const char *input;      /**< 待净化输入（必填） */
} cupolas_sanitize_params_t;

/** @brief cupolas.execute_command 参数 */
typedef struct {
    const char *command;    /**< 命令路径（必填） */
    char **argv;            /**< 参数数组（NULL 结尾，argv[0] 为命令名） */
} cupolas_execute_command_params_t;

/** @brief cupolas.add_rule 参数 */
typedef struct {
    const char *agent_id;   /**< 主体匹配模式（NULL 或 "*" 通配） */
    const char *action;     /**< 动作匹配模式（NULL 或 "*" 通配） */
    const char *resource;   /**< 资源 glob 模式 */
    int allow;              /**< 1 允许 / 0 拒绝 */
    int priority;           /**< 优先级，值越大越优先 */
} cupolas_add_rule_params_t;

/* ==================== 方法结果结构体 ==================== */

/** @brief cupolas.check_permission 结果 */
typedef struct {
    int allowed;            /**< 1 允许 / 0 拒绝 */
    int err;                /**< cupolas 底层返回码（<0 为错误） */
} cupolas_check_permission_result_t;

/** @brief cupolas.sanitize 结果 */
typedef struct {
    char *sanitized;        /**< 净化结果字符串（调用方负责 AIRY_FREE） */
    int err;                /**< 底层返回码（0 正常，非 0 时 sanitized 仍可能有效） */
} cupolas_sanitize_result_t;

/** @brief cupolas.execute_command 结果 */
typedef struct {
    int exit_code;          /**< 命令退出码 */
    char *stdout_buf;       /**< 标准输出（调用方负责 AIRY_FREE） */
    char *stderr_buf;       /**< 标准错误（调用方负责 AIRY_FREE） */
    int err;                /**< 底层返回码（0 成功） */
} cupolas_execute_command_result_t;

/** @brief cupolas.add_rule 结果 */
typedef struct {
    int added;              /**< 1 添加成功 / 0 失败 */
    int err;                /**< 底层返回码 */
} cupolas_add_rule_result_t;

/* ==================== 服务生命周期 ==================== */

/**
 * @brief 创建 cupolas 服务实例
 * @param config_path cupolas 配置文件路径（NULL 使用默认配置）
 * @return 服务实例，失败返回 NULL
 * @note cupolas 为进程级单例库（cupolas_init），本实例仅承载统计与配置元数据；
 *       实际模块初始化由 main() 调用 daemon_cupolas_init() 完成
 */
cupolas_service_t *cupolas_service_create(const char *config_path);

/**
 * @brief 销毁 cupolas 服务实例
 * @param svc 服务实例（可 NULL）
 */
void cupolas_service_destroy(cupolas_service_t *svc);

/* ==================== 方法实现 ==================== */

/**
 * @brief 权限裁决：检查主体对资源执行动作的权限
 * @return AIRY_SUCCESS 成功，*out 输出允许结果；否则错误码
 */
int cupolas_service_check_permission(cupolas_service_t *svc,
                                     const cupolas_check_permission_params_t *params,
                                     cupolas_check_permission_result_t *out);

/**
 * @brief 输入净化：净化输入字符串（HTML/SQL/Shell 注入防护）
 * @return AIRY_SUCCESS 成功（净化或原样放行），*out 输出结果；否则错误码
 */
int cupolas_service_sanitize(cupolas_service_t *svc,
                             const cupolas_sanitize_params_t *params,
                             cupolas_sanitize_result_t *out);

/**
 * @brief 隔离工位命令执行：在 cupolas workbench 中执行命令
 * @return AIRY_SUCCESS 成功（无论命令退出码），*out 输出退出码与输出；否则错误码
 */
int cupolas_service_execute_command(cupolas_service_t *svc,
                                    const cupolas_execute_command_params_t *params,
                                    cupolas_execute_command_result_t *out);

/**
 * @brief 动态添加权限规则
 * @return AIRY_SUCCESS 成功，*out 输出添加结果；否则错误码
 */
int cupolas_service_add_rule(cupolas_service_t *svc,
                             const cupolas_add_rule_params_t *params,
                             cupolas_add_rule_result_t *out);

/**
 * @brief 刷新审计日志（落盘所有 pending 审计记录）
 * @return AIRY_SUCCESS 成功
 */
int cupolas_service_audit_flush(cupolas_service_t *svc);

/**
 * @brief 获取服务统计 JSON 字符串（真实统计：版本/运行时长/权限检查数/净化数）
 * @return JSON 字符串（调用方负责 AIRY_FREE），失败返回 NULL
 */
char *cupolas_service_get_stats_json(cupolas_service_t *svc);

/* ---------- 结果释放辅助 ---------- */

void cupolas_sanitize_result_free(cupolas_sanitize_result_t *out);
void cupolas_execute_command_result_free(cupolas_execute_command_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_CUPOLAS_SERVICE_H */
