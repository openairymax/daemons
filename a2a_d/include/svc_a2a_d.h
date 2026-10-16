/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.15.0 (L3 SSoT)；改 .manifest 后重跑 --gen。 */

#ifndef SVC_A2A_D_H
#define SVC_A2A_D_H

#include "platform.h"
#include "daemon_main.h"
#include "airy_defaults.h"

#include <cjson/cJSON.h>

/* 端点常量（wire 契约，与 .manifest rpc 段一致） */
#define A2A_D_SOCKET_UNIX airy_runtime_dir_socket("a2a.sock")
#define A2A_D_SOCKET_WIN "\\\\.\\pipe\\airy_a2a"
/* TCP 口为 SSoT 引用，真值唯一定义于 airy_defaults.h */
#define A2A_D_TCP_PORT AIRY_PORT_A2A_D
#define A2A_D_MAX_BUFFER 65536

/* 端点解析钩子（.manifest endpoint=apply 派生的机制壳；机制体单点
 * 在 daemon_ep_base/daemon_ep_apply，daemon_cfg_file.c）。cmdline
 * use_tcp 融合与监听地址日志由 daemon_boot 机制层承载。 */
static inline void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp)
{
    daemon_ep_apply(ep, cmdline_tcp);
}

/* 生命周期钩子（实现: src/svc.c）。prepare/destroy 恒有策略；
 * activate/attach/teardown 无策略需求者（noop_hooks，0.1.19
 * §80）由机制层 svc_<hook>_noop 缺省（daemon_main.h），
 * 此处不发声明，svc.c 不维护空桩副本。activate 收到事件驱动与
 * SD bootstrap 句柄；attach 为 SVC_METHODS 落库后的动态注册
 * 出口（dispatcher 为 method_dispatcher_t）。 */
int svc_prepare(const char *config_path);
void svc_destroy(void);

/* RPC 方法清单（唯一声明源，源自 .manifest rpc.methods）。
 * RPC_METHODS 只含策略方法；METHODS 追加协议保留 shutdown
 * （main.c DAEMON_DECLARE_SHUTDOWN_METHOD 生成为 static inline，
 * 不可对外发原型）。main.c 以 DAEMON_METHOD_ENTRY 展开 METHODS
 * 为 daemon_method_entry_t[]，本头以 DAEMON_M_DECL 展开
 * RPC_METHODS 为 m_<method> 原型（逐户声明块副本消解）。 */
#define SVC_A2A_D_RPC_METHODS(X) \
    X("register_agent", m_register_agent) \
    X("unregister_agent", m_unregister_agent) \
    X("discover_agents", m_discover_agents) \
    X("create_task", m_create_task) \
    X("update_task", m_update_task) \
    X("cancel_task", m_cancel_task) \
    X("get_task", m_get_task) \
    X("send_message", m_send_message) \
    X("count", m_count) \
    X("send", m_send) \
    X("receive", m_receive) \
    X("health_check", m_health_check) \
    X("get_stats", m_get_stats)

#define SVC_A2A_D_METHODS(X) \
    SVC_A2A_D_RPC_METHODS(X) \
    X("shutdown", on_shutdown_method_a2a_d)

/* RPC handler 族原型（实现: src/svc.c，壳经 DAEMON_RPC_SHELL 族）。 */
SVC_A2A_D_RPC_METHODS(DAEMON_M_DECL)

#endif /* SVC_A2A_D_H */
