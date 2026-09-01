// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file cupolas_d_internal.h
 * @brief cupolas_d 拆分文件间的共享声明（RPC 方法适配器 / 服务句柄）。
 *        daemon_main.h 生成的样板（g_running_cupolas_d 等）与 daemon 配置
 *        装配（g_config/load_daemon_config）仍保持 static 于 main.c 内，
 *        此处仅声明跨文件符号。
 */

#ifndef AIRY_RT_DAEMON_CUPOLAS_D_INTERNAL_H
#define AIRY_RT_DAEMON_CUPOLAS_D_INTERNAL_H

#include "cupolas_service.h"

#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 服务句柄（main.c 定义，RPC 处理器文件引用） ---- */
extern cupolas_service_t *g_service;

/* ---- 动态策略引擎句柄（PDP，main.c 创建/销毁，policy RPC 引用） ---- */
extern struct dpolicy_engine_s *g_dpolicy;

/* ---- RPC 方法适配器（cupolas_rpc_*.c，main() 注册到 dispatcher） ---- */
void on_check_permission_method(cJSON *params, int id, void *user_data);
void on_sanitize_method(cJSON *params, int id, void *user_data);
void on_execute_command_method(cJSON *params, int id, void *user_data);
void on_add_rule_method(cJSON *params, int id, void *user_data);
void on_audit_flush_method(cJSON *params, int id, void *user_data);
void on_get_stats_method(cJSON *params, int id, void *user_data);
void on_health_check_method(cJSON *params, int id, void *user_data);
void on_vault_store_method(cJSON *params, int id, void *user_data);
void on_vault_retrieve_method(cJSON *params, int id, void *user_data);
void on_vault_delete_method(cJSON *params, int id, void *user_data);
void on_vault_list_method(cJSON *params, int id, void *user_data);
void on_vault_rotate_method(cJSON *params, int id, void *user_data);
void on_net_add_rule_method(cJSON *params, int id, void *user_data);
void on_net_check_access_method(cJSON *params, int id, void *user_data);
void on_net_get_stats_method(cJSON *params, int id, void *user_data);
void on_entitlements_load_method(cJSON *params, int id, void *user_data);
void on_entitlements_check_method(cJSON *params, int id, void *user_data);
void on_policy_load_method(cJSON *params, int id, void *user_data);
void on_policy_activate_method(cJSON *params, int id, void *user_data);
void on_policy_rollback_method(cJSON *params, int id, void *user_data);
void on_policy_status_method(cJSON *params, int id, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_CUPOLAS_D_INTERNAL_H */
