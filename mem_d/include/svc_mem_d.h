/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.11.0 (L3 SSoT) 生成。
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

/* 端点常量（wire 契约，与 .manifest rpc 段一致；svc_endpoint 缺省基线） */
#define MEM_D_SOCKET_UNIX airy_runtime_dir_socket("mem.sock")
#define MEM_D_SOCKET_WIN "\\\\.\\pipe\\airy_mem"
/* TCP 口为 SSoT 引用，真值唯一定义于 airy_defaults.h */
#define MEM_D_TCP_PORT AIRY_PORT_MEM_D
#define MEM_D_MAX_BUFFER 65536

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
void m_write(cJSON *params, int id, void *user_data);
void m_search(cJSON *params, int id, void *user_data);
void m_get(cJSON *params, int id, void *user_data);
void m_delete(cJSON *params, int id, void *user_data);
void m_count(cJSON *params, int id, void *user_data);
void m_recent(cJSON *params, int id, void *user_data);
void m_evolve(cJSON *params, int id, void *user_data);
void m_health_check(cJSON *params, int id, void *user_data);
void m_get_stats(cJSON *params, int id, void *user_data);
void m_kb_ingest(cJSON *params, int id, void *user_data);
void m_kb_search(cJSON *params, int id, void *user_data);
void m_kb_delete(cJSON *params, int id, void *user_data);
void m_kb_list(cJSON *params, int id, void *user_data);
void m_cache_put(cJSON *params, int id, void *user_data);
void m_cache_get(cJSON *params, int id, void *user_data);
void m_cache_del(cJSON *params, int id, void *user_data);
void m_cache_stats(cJSON *params, int id, void *user_data);
void m_ledger_append(cJSON *params, int id, void *user_data);
void m_ledger_window(cJSON *params, int id, void *user_data);
void m_ledger_budget(cJSON *params, int id, void *user_data);
void m_ledger_mark(cJSON *params, int id, void *user_data);
void m_ledger_history(cJSON *params, int id, void *user_data);
void m_ledger_stats(cJSON *params, int id, void *user_data);
void m_compress(cJSON *params, int id, void *user_data);

/* RPC 方法表清单（唯一声明源，源自 .manifest rpc.methods）。
 * main.c 以 X 宏展开为 daemon_method_entry_t[]：
 *   #define X(n, f) {(n), (f)},
 *   static const daemon_method_entry_t T[] = { SVC_MEM_D_METHODS(X) };
 * 装配行数与方法数解耦（机制层装配，策略数据在此单点维护）。 */
#define SVC_MEM_D_METHODS(X) \
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
    X("compress", m_compress) \
    X("shutdown", on_shutdown_method_mem_d)

#endif /* SVC_MEM_D_H */
