/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/* @generated DO NOT EDIT — daemon_gen.py v1.8.0 (L3 SSoT) 生成。
 * 机制层装配；策略层在 src/svc.c 与 modules（手写域）。
 * 改 .manifest 后: python3 agentrt/tools/codegen/daemon_gen.py --gen
 */

#ifndef SVC_AGENT_D_H
#define SVC_AGENT_D_H

#include "platform.h"
#include "daemon_main.h"
#include "airy_defaults.h"

#include <cjson/cJSON.h>

/* 端点常量（wire 契约，与 .manifest rpc 段一致；svc_endpoint 缺省基线） */
#define AGENT_D_SOCKET_UNIX airy_runtime_dir_socket("agent.sock")
#define AGENT_D_SOCKET_WIN "\\\\.\\pipe\\airy_agent"
/* TCP 口为 SSoT 引用，真值唯一定义于 airy_defaults.h */
#define AGENT_D_TCP_PORT AIRY_PORT_AGENT_D
#define AGENT_D_MAX_BUFFER 65536

/* 端点解析钩子：常量户回填上方基线；可配置户在 svc.c 完成
 * config/env 覆盖后与 cmdline use_tcp 融合。实现: src/svc.c。 */
void svc_endpoint(daemon_endpoint_t *ep, int cmdline_tcp);

/* 生命周期钩子（实现: src/svc.c）；activate 收到事件驱动句柄与
 * SD bootstrap 句柄，供事件耦合激活策略（如监控采样线程）与
 * manifest deps 驱动的依赖探测健康面使用。 */
int svc_prepare(const char *config_path);
int svc_activate(daemon_event_driver_t *driver, daemon_bootstrap_sd_t *bsd);
void svc_teardown(void);
void svc_destroy(void);

/* 策略层附加装配挂点：静态注册表（SVC_METHODS）落库后的动态
 * 注册出口（如 roadmap.* 方法族）。实现: src/svc.c；无附加
 * 注册的户提供空实现。dispatcher 为 method_dispatcher_t。 */
void svc_attach(void *dispatcher);

/* RPC handler 族（实现: src/svc.c）。签名对齐 method_fn；
 * 命名 m_<method>，与 .manifest rpc.methods 一一对应。 */
void m_spawn(cJSON *params, int id, void *user_data);
void m_terminate(cJSON *params, int id, void *user_data);
void m_invoke(cJSON *params, int id, void *user_data);
void m_cancel(cJSON *params, int id, void *user_data);
void m_list(cJSON *params, int id, void *user_data);
void m_count(cJSON *params, int id, void *user_data);
void m_run(cJSON *params, int id, void *user_data);
void m_run_cancel(cJSON *params, int id, void *user_data);
void m_run_stream(cJSON *params, int id, void *user_data);
void m_health_check(cJSON *params, int id, void *user_data);
void m_get_stats(cJSON *params, int id, void *user_data);
void m_vocab(cJSON *params, int id, void *user_data);

/* RPC 方法表清单（唯一声明源，源自 .manifest rpc.methods）。
 * main.c 以 X 宏展开为 daemon_method_entry_t[]：
 *   #define X(n, f) {(n), (f)},
 *   static const daemon_method_entry_t T[] = { SVC_AGENT_D_METHODS(X) };
 * 装配行数与方法数解耦（机制层装配，策略数据在此单点维护）。 */
#define SVC_AGENT_D_METHODS(X) \
    X("spawn", m_spawn) \
    X("terminate", m_terminate) \
    X("invoke", m_invoke) \
    X("cancel", m_cancel) \
    X("list", m_list) \
    X("count", m_count) \
    X("run", m_run) \
    X("run_cancel", m_run_cancel) \
    X("run_stream", m_run_stream) \
    X("health_check", m_health_check) \
    X("get_stats", m_get_stats) \
    X("vocab", m_vocab) \
    X("shutdown", on_shutdown_method_agent_d)

#endif /* SVC_AGENT_D_H */
