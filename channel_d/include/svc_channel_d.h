/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.3.0 (L3 SSoT) 生成。
 * 机制层装配；策略层在 src/svc.c 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#ifndef SVC_CHANNEL_D_H
#define SVC_CHANNEL_D_H

#include "platform.h"
#include "daemon_main.h"

#include <cjson/cJSON.h>

/* 端点常量（wire 契约，与 .manifest rpc 段一致；svc_endpoint 缺省基线） */
#define CHANNEL_D_SOCKET_UNIX airy_runtime_dir_socket("channel.sock")
#define CHANNEL_D_SOCKET_WIN "\\\\.\\pipe\\airy_channel"
#define CHANNEL_D_TCP_PORT 8094
#define CHANNEL_D_MAX_BUFFER 65536

/* 端点解析钩子：常量户回填上方基线；可配置户在 svc.c 完成
 * config/env 覆盖后与 cmdline use_tcp 融合。实现: src/svc.c。
 * 命名豁免 15 字节（机械对齐户名）。 */
void svc_endpoint_channel_d(daemon_endpoint_t *ep, int cmdline_tcp);

/* 生命周期钩子（实现: src/svc.c）；activate 收到事件驱动句柄，
 * 供与事件循环耦合的激活策略使用（如监控采样线程）。 */
int svc_prepare_channel_d(const char *config_path);
int svc_activate_channel_d(daemon_event_driver_t *driver);
void svc_teardown_channel_d(void);
void svc_destroy_channel_d(void);

/* 策略层附加装配挂点：静态注册表（SVC_METHODS）落库后的动态
 * 注册出口（如 roadmap.* 方法族）。实现: src/svc.c；无附加
 * 注册的户提供空实现。dispatcher 为 method_dispatcher_t。
 * 命名 <action>_<daemon> 豁免 15 字节（机械对齐户名）。 */
void svc_attach_channel_d(void *dispatcher);

/* RPC handler 族（实现: src/svc.c）。签名对齐 method_fn；命名
 * <method>_<daemon> 三段式机械对齐注册表，豁免 15 字节。 */
void svc_on_ping_channel_d(cJSON *params, int id, void *user_data);
void svc_on_list_channel_d(cJSON *params, int id, void *user_data);
void svc_on_open_channel_d(cJSON *params, int id, void *user_data);
void svc_on_close_channel_d(cJSON *params, int id, void *user_data);
void svc_on_send_channel_d(cJSON *params, int id, void *user_data);
void svc_on_health_channel_d(cJSON *params, int id, void *user_data);
void svc_on_health_check_channel_d(cJSON *params, int id, void *user_data);
void svc_on_get_stats_channel_d(cJSON *params, int id, void *user_data);

#endif /* SVC_CHANNEL_D_H */
