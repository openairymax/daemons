/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.14.0 (L3 SSoT) 生成。
 * manifest 派生产物；装配机制在 daemons/common，策略在 src/svc.c
 * 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#ifndef SVC_MEM_D_H
#define SVC_MEM_D_H

#include "platform.h"
#include "daemon_main.h"
#include "airy_defaults.h"

#include <cjson/cJSON.h>

/* 端点常量（wire 契约，与 .manifest rpc 段一致） */
#define MEM_D_SOCKET_UNIX airy_runtime_dir_socket("mem.sock")
#define MEM_D_SOCKET_WIN "\\\\.\\pipe\\airy_mem"
/* TCP 口为 SSoT 引用，真值唯一定义于 airy_defaults.h */
#define MEM_D_TCP_PORT AIRY_PORT_MEM_D
#define MEM_D_MAX_BUFFER 65536

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
#define SVC_MEM_D_RPC_METHODS(X) \
    X("write", m_write) \
    X("search", m_search) \
    X("get", m_get) \
    X("delete", m_delete) \
    X("count", m_count) \
    X("recent", m_recent) \
    X("evolve", m_evolve) \
    X("health_check", m_health_check) \
    X("get_stats", m_get_stats) \
    X("kb_ingest", m_kb_ingest) \
    X("kb_search", m_kb_search) \
    X("kb_delete", m_kb_delete) \
    X("kb_list", m_kb_list) \
    X("cache_put", m_cache_put) \
    X("cache_get", m_cache_get) \
    X("cache_del", m_cache_del) \
    X("cache_stats", m_cache_stats) \
    X("ledger_append", m_ledger_append) \
    X("ledger_window", m_ledger_window) \
    X("ledger_budget", m_ledger_budget) \
    X("ledger_mark", m_ledger_mark) \
    X("ledger_history", m_ledger_history) \
    X("ledger_stats", m_ledger_stats) \
    X("compress", m_compress)

#define SVC_MEM_D_METHODS(X) \
    SVC_MEM_D_RPC_METHODS(X) \
    X("shutdown", on_shutdown_method_mem_d)

/* RPC handler 族原型（实现: src/svc.c，壳经 DAEMON_RPC_SHELL 族）。 */
SVC_MEM_D_RPC_METHODS(DAEMON_M_DECL)

#endif /* SVC_MEM_D_H */
