/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.13.0 (L3 SSoT) 生成。
 * manifest 派生产物；装配机制在 daemons/common，策略在 src/svc.c
 * 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#ifndef SVC_MONIT_D_H
#define SVC_MONIT_D_H

#include "platform.h"
#include "daemon_main.h"
#include "airy_defaults.h"

#include <cjson/cJSON.h>

/* 端点常量（wire 契约，与 .manifest rpc 段一致；svc_endpoint 缺省基线） */
#define MONIT_D_SOCKET_UNIX airy_runtime_dir_socket("monit.sock")
#define MONIT_D_SOCKET_WIN "\\\\.\\pipe\\airy_monit"
/* TCP 口为 SSoT 引用，真值唯一定义于 airy_defaults.h */
#define MONIT_D_TCP_PORT AIRY_PORT_MONIT_D
#define MONIT_D_MAX_BUFFER 65536

/* 端点解析钩子：常量户回填上方基线；可配置户在 svc.c 完成
 * config/env 覆盖后与 cmdline use_tcp 融合。实现: src/svc.c。 */
void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp);

/* 生命周期钩子（实现: src/svc.c）。prepare/destroy 恒有策略；
 * activate/attach/teardown 无策略需求者（noop_hooks，0.1.19
 * §80）由机制层 daemon_svc_<hook>_noop 缺省（daemon_main.h），
 * 此处不发声明，svc.c 不维护空桩副本。activate 收到事件驱动与
 * SD bootstrap 句柄；attach 为 SVC_METHODS 落库后的动态注册
 * 出口（dispatcher 为 method_dispatcher_t）。 */
int svc_prepare(const char *config_path);
int svc_activate(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd);
void svc_attach(void *dispatcher);
void svc_destroy(void);

/* RPC handler 族（实现: src/svc.c）。签名对齐 method_fn；
 * 命名 m_<method>，与 .manifest rpc.methods 一一对应。 */
void m_record_metric(cJSON *params, int id, void *user_data);
void m_get_metrics(cJSON *params, int id, void *user_data);
void m_trigger_alert(cJSON *params, int id, void *user_data);
void m_get_alerts(cJSON *params, int id, void *user_data);
void m_health_check(cJSON *params, int id, void *user_data);
void m_generate_report(cJSON *params, int id, void *user_data);
void m_heartbeat(cJSON *params, int id, void *user_data);
void m_metrics(cJSON *params, int id, void *user_data);
void m_alert_raise(cJSON *params, int id, void *user_data);
void m_alert_resolve(cJSON *params, int id, void *user_data);
void m_get_stats(cJSON *params, int id, void *user_data);

/* RPC 方法表清单（唯一声明源，源自 .manifest rpc.methods）。
 * main.c 以 X 宏展开为 daemon_method_entry_t[]：
 *   #define X(n, f) {(n), (f)},
 *   static const daemon_method_entry_t T[] = { SVC_MONIT_D_METHODS(X) };
 * 装配行数与方法数解耦（机制层装配，策略数据在此单点维护）。 */
#define SVC_MONIT_D_METHODS(X) \
    X("record_metric", m_record_metric) \
    X("get_metrics", m_get_metrics) \
    X("trigger_alert", m_trigger_alert) \
    X("get_alerts", m_get_alerts) \
    X("health_check", m_health_check) \
    X("generate_report", m_generate_report) \
    X("heartbeat", m_heartbeat) \
    X("metrics", m_metrics) \
    X("alert_raise", m_alert_raise) \
    X("alert_resolve", m_alert_resolve) \
    X("get_stats", m_get_stats) \
    X("shutdown", on_shutdown_method_monit_d)

#endif /* SVC_MONIT_D_H */
