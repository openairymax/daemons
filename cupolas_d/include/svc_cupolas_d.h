/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.4.0 (L3 SSoT) 生成。
 * 机制层装配；策略层在 src/svc.c 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#ifndef SVC_CUPOLAS_D_H
#define SVC_CUPOLAS_D_H

#include "platform.h"
#include "daemon_main.h"

#include <cjson/cJSON.h>

/* 端点常量（wire 契约，与 .manifest rpc 段一致；svc_endpoint 缺省基线） */
#define CUPOLAS_D_SOCKET_UNIX airy_runtime_dir_socket("cupolas.sock")
#define CUPOLAS_D_SOCKET_WIN "\\\\.\\pipe\\airy_cupolas"
#define CUPOLAS_D_TCP_PORT 8089
#define CUPOLAS_D_MAX_BUFFER 65536

/* 端点解析钩子：常量户回填上方基线；可配置户在 svc.c 完成
 * config/env 覆盖后与 cmdline use_tcp 融合。实现: src/svc.c。
 * 命名豁免 15 字节（机械对齐户名）。 */
void svc_endpoint_cupolas_d(daemon_endpoint_t *ep, int cmdline_tcp);

/* 生命周期钩子（实现: src/svc.c）；activate 收到事件驱动句柄与
 * SD bootstrap 句柄，供事件耦合激活策略（如监控采样线程）与
 * manifest deps 驱动的依赖探测健康面使用。 */
int svc_prepare_cupolas_d(const char *config_path);
int svc_activate_cupolas_d(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd);
void svc_teardown_cupolas_d(void);
void svc_destroy_cupolas_d(void);

/* 策略层附加装配挂点：静态注册表（SVC_METHODS）落库后的动态
 * 注册出口（如 roadmap.* 方法族）。实现: src/svc.c；无附加
 * 注册的户提供空实现。dispatcher 为 method_dispatcher_t。
 * 命名 <action>_<daemon> 豁免 15 字节（机械对齐户名）。 */
void svc_attach_cupolas_d(void *dispatcher);

/* RPC handler 族（实现: src/svc.c）。签名对齐 method_fn；命名
 * <method>_<daemon> 三段式机械对齐注册表，豁免 15 字节。 */
void svc_on_check_permission_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_sanitize_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_execute_command_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_add_rule_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_audit_flush_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_health_check_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_get_stats_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_vault_store_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_vault_retrieve_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_vault_delete_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_vault_list_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_vault_rotate_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_net_add_rule_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_net_check_access_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_net_get_stats_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_entitlements_load_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_entitlements_check_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_policy_load_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_policy_activate_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_policy_rollback_cupolas_d(cJSON *params, int id, void *user_data);
void svc_on_policy_status_cupolas_d(cJSON *params, int id, void *user_data);

#endif /* SVC_CUPOLAS_D_H */
