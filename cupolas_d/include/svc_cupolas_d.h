/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.12.0 (L3 SSoT) 生成。
 * manifest 派生产物；装配机制在 daemons/common，策略在 src/svc.c
 * 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#ifndef SVC_CUPOLAS_D_H
#define SVC_CUPOLAS_D_H

#include "platform.h"
#include "daemon_main.h"
#include "airy_defaults.h"

#include <cjson/cJSON.h>

/* 端点常量（wire 契约，与 .manifest rpc 段一致；svc_endpoint 缺省基线） */
#define CUPOLAS_D_SOCKET_UNIX airy_runtime_dir_socket("cupolas.sock")
#define CUPOLAS_D_SOCKET_WIN "\\\\.\\pipe\\airy_cupolas"
/* TCP 口为 SSoT 引用，真值唯一定义于 airy_defaults.h */
#define CUPOLAS_D_TCP_PORT AIRY_PORT_CUPOLAS_D
#define CUPOLAS_D_MAX_BUFFER 65536

/* 端点解析钩子：常量户回填上方基线；可配置户在 svc.c 完成
 * config/env 覆盖后与 cmdline use_tcp 融合。实现: src/svc.c。 */
void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp);

/* 生命周期钩子（实现: src/svc.c）；激活钩子无策略需求，由
 * 机制层 daemon_svc_noop 缺省（daemon_main.h，0.1.19 §80），
 * svc.c 不再维护空桩副本。 */
int svc_prepare(const char *config_path);
void svc_teardown(void);
void svc_destroy(void);

/* 策略层附加装配挂点：静态注册表（SVC_METHODS）落库后的动态
 * 注册出口（如 roadmap.* 方法族）。实现: src/svc.c；无附加
 * 注册的户提供空实现。dispatcher 为 method_dispatcher_t。 */
void svc_attach(void *dispatcher);

/* RPC handler 族（实现: src/svc.c）。签名对齐 method_fn；
 * 命名 m_<method>，与 .manifest rpc.methods 一一对应。 */
void m_check_permission(cJSON *params, int id, void *user_data);
void m_sanitize(cJSON *params, int id, void *user_data);
void m_execute_command(cJSON *params, int id, void *user_data);
void m_add_rule(cJSON *params, int id, void *user_data);
void m_audit_flush(cJSON *params, int id, void *user_data);
void m_health_check(cJSON *params, int id, void *user_data);
void m_get_stats(cJSON *params, int id, void *user_data);
void m_vault_store(cJSON *params, int id, void *user_data);
void m_vault_retrieve(cJSON *params, int id, void *user_data);
void m_vault_delete(cJSON *params, int id, void *user_data);
void m_vault_list(cJSON *params, int id, void *user_data);
void m_vault_rotate(cJSON *params, int id, void *user_data);
void m_net_add_rule(cJSON *params, int id, void *user_data);
void m_net_check_access(cJSON *params, int id, void *user_data);
void m_net_get_stats(cJSON *params, int id, void *user_data);
void m_entitlements_load(cJSON *params, int id, void *user_data);
void m_entitlements_check(cJSON *params, int id, void *user_data);
void m_policy_load(cJSON *params, int id, void *user_data);
void m_policy_activate(cJSON *params, int id, void *user_data);
void m_policy_rollback(cJSON *params, int id, void *user_data);
void m_policy_status(cJSON *params, int id, void *user_data);

/* RPC 方法表清单（唯一声明源，源自 .manifest rpc.methods）。
 * main.c 以 X 宏展开为 daemon_method_entry_t[]：
 *   #define X(n, f) {(n), (f)},
 *   static const daemon_method_entry_t T[] = { SVC_CUPOLAS_D_METHODS(X) };
 * 装配行数与方法数解耦（机制层装配，策略数据在此单点维护）。 */
#define SVC_CUPOLAS_D_METHODS(X) \
    X("check_permission", m_check_permission) \
    X("sanitize", m_sanitize) \
    X("execute_command", m_execute_command) \
    X("add_rule", m_add_rule) \
    X("audit_flush", m_audit_flush) \
    X("health_check", m_health_check) \
    X("get_stats", m_get_stats) \
    X("vault_store", m_vault_store) \
    X("vault_retrieve", m_vault_retrieve) \
    X("vault_delete", m_vault_delete) \
    X("vault_list", m_vault_list) \
    X("vault_rotate", m_vault_rotate) \
    X("net_add_rule", m_net_add_rule) \
    X("net_check_access", m_net_check_access) \
    X("net_get_stats", m_net_get_stats) \
    X("entitlements_load", m_entitlements_load) \
    X("entitlements_check", m_entitlements_check) \
    X("policy_load", m_policy_load) \
    X("policy_activate", m_policy_activate) \
    X("policy_rollback", m_policy_rollback) \
    X("policy_status", m_policy_status) \
    X("shutdown", on_shutdown_method_cupolas_d)

#endif /* SVC_CUPOLAS_D_H */
