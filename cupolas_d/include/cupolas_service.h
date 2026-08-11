/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

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
 */

#ifndef AIRY_RT_DAEMON_CUPOLAS_SERVICE_H
#define AIRY_RT_DAEMON_CUPOLAS_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cupolas_service cupolas_service_t;


typedef struct {
    const char *agent_id;
    const char *action;
    const char *resource;
    const char *context;
} cupolas_check_permission_params_t;


typedef struct {
    const char *input;
} cupolas_sanitize_params_t;


typedef struct {
    const char *command;
    char **argv;
} cupolas_execute_command_params_t;


typedef struct {
    const char *agent_id;
    const char *action;
    const char *resource;
    int allow;
    int priority;
} cupolas_add_rule_params_t;


typedef struct {
    int allowed;
    int err;
} cupolas_check_permission_result_t;


typedef struct {
    char *sanitized;
    int err;
} cupolas_sanitize_result_t;


typedef struct {
    int exit_code;
    char *stdout_buf;
    char *stderr_buf;
    int err;
} cupolas_execute_command_result_t;


typedef struct {
    int added;
    int err;
} cupolas_add_rule_result_t;


/* ==================== 凭据保险库（cupolas.vault_*） ==================== */

typedef struct {
    const char *cred_id;
    int cred_type; /* cupolas_vault_cred_type_t */
    const uint8_t *data;
    size_t data_len;
    const char *agent_id;
} cupolas_vault_store_params_t;

typedef struct {
    int stored;
    int err;
} cupolas_vault_store_result_t;

typedef struct {
    const char *cred_id;
    const char *agent_id;
} cupolas_vault_retrieve_params_t;

typedef struct {
    uint8_t *data; /* 调用方用 cupolas_vault_retrieve_result_free 释放 */
    size_t data_len;
    int err;
} cupolas_vault_retrieve_result_t;

typedef struct {
    const char *cred_id;
    const char *agent_id;
} cupolas_vault_delete_params_t;

typedef struct {
    int deleted;
    int err;
} cupolas_vault_delete_result_t;

typedef struct {
    const char *cred_group;
    int strategy; /* cupolas_vault_rotation_strategy_t */
} cupolas_vault_rotate_params_t;

typedef struct {
    char *selected_id; /* 调用方 AIRY_FREE */
    int err;
} cupolas_vault_rotate_result_t;


/* ==================== 网络规则（cupolas.net_*） ==================== */

typedef struct {
    const char *rule_id;
    const char *description;
    const char *src_ip;
    const char *dst_ip;
    const char *src_port;
    const char *dst_port;
    int protocol;  /* cupolas_proto_t */
    int direction; /* cupolas_direction_t */
    int action;    /* cupolas_fw_action_t */
    int priority;
} cupolas_net_add_rule_params_t;

typedef struct {
    int added;
    int err;
} cupolas_net_add_rule_result_t;

typedef struct {
    const char *host;
    uint16_t port;
    int protocol;      /* cupolas_proto_t */
    const char *direction; /* "inbound" / "outbound" */
} cupolas_net_check_access_params_t;

typedef struct {
    int allowed;
    int err;
} cupolas_net_check_access_result_t;


/* ==================== 权能清单（cupolas.entitlements_*） ==================== */

typedef struct {
    const char *yaml_path;
} cupolas_entitlements_load_params_t;

typedef struct {
    int loaded;
    int err;
} cupolas_entitlements_load_result_t;

typedef struct {
    const char *kind; /* "fs" | "net" | "ipc" | "syscall" | "capability" | "vault" */
    const char *param1;
    const char *param2;
} cupolas_entitlements_check_params_t;

typedef struct {
    int allowed;
    int err;
} cupolas_entitlements_check_result_t;


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
int cupolas_service_sanitize(cupolas_service_t *svc, const cupolas_sanitize_params_t *params,
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
int cupolas_service_add_rule(cupolas_service_t *svc, const cupolas_add_rule_params_t *params,
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


void cupolas_sanitize_result_free(cupolas_sanitize_result_t *out);
void cupolas_execute_command_result_free(cupolas_execute_command_result_t *out);


/* ==================== 凭据保险库服务方法 ==================== */

int cupolas_service_vault_store(cupolas_service_t *svc, const cupolas_vault_store_params_t *params,
                                cupolas_vault_store_result_t *out);
int cupolas_service_vault_retrieve(cupolas_service_t *svc,
                                   const cupolas_vault_retrieve_params_t *params,
                                   cupolas_vault_retrieve_result_t *out);
void cupolas_vault_retrieve_result_free(cupolas_vault_retrieve_result_t *out);
int cupolas_service_vault_delete(cupolas_service_t *svc,
                                 const cupolas_vault_delete_params_t *params,
                                 cupolas_vault_delete_result_t *out);
int cupolas_service_vault_rotate(cupolas_service_t *svc,
                                 const cupolas_vault_rotate_params_t *params,
                                 cupolas_vault_rotate_result_t *out);
char *cupolas_service_vault_list_json(cupolas_service_t *svc, int cred_type);


/* ==================== 网络规则服务方法 ==================== */

int cupolas_service_net_add_rule(cupolas_service_t *svc,
                                 const cupolas_net_add_rule_params_t *params,
                                 cupolas_net_add_rule_result_t *out);
int cupolas_service_net_check_access(cupolas_service_t *svc,
                                     const cupolas_net_check_access_params_t *params,
                                     cupolas_net_check_access_result_t *out);
char *cupolas_service_net_get_stats_json(cupolas_service_t *svc);


/* ==================== 权能清单服务方法 ==================== */

int cupolas_service_entitlements_load(cupolas_service_t *svc,
                                      const cupolas_entitlements_load_params_t *params,
                                      cupolas_entitlements_load_result_t *out);
int cupolas_service_entitlements_check(cupolas_service_t *svc,
                                       const cupolas_entitlements_check_params_t *params,
                                       cupolas_entitlements_check_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_CUPOLAS_SERVICE_H */
