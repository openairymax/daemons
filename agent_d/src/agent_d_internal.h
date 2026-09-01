// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_d_internal.h
 * @brief agent_d 拆分文件间的共享声明（2026-08-27）。
 *
 * main.c 按单一职责拆分为三个文件：
 *   - main.c            入口引导 + daemon 配置装配 + 方法注册
 *   - agent_d_rpc.c     agent.* RPC 方法（spawn/terminate/invoke/cancel/...）
 *   - agent_d_monitor.c 空闲回收线程 + 性能采样线程（POSIX）
 * daemon_main.h 生成的样板（g_running_agent_d / signal_handler_agent_d 等）
 * 仍保持 static 于 main.c 内；此处仅声明跨文件符号。
 */

#ifndef AIRY_RT_DAEMON_AGENT_D_INTERNAL_H
#define AIRY_RT_DAEMON_AGENT_D_INTERNAL_H

#include "agent_service.h"

#include "daemon_event_driver.h"

#include <cjson/cJSON.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 服务句柄 / 启动时间 / daemon 配置（main.c 定义，RPC 与监控文件引用） ---- */
extern agent_service_t *g_service;
extern uint64_t g_start_time;

typedef struct {
    char *socket_path;
    char *tcp_host;
    uint16_t tcp_port;
    int use_tcp;
    int max_clients;
    size_t max_agents;
} agent_daemon_config_t;

extern agent_daemon_config_t g_config;

/* ---- 监控线程域（agent_d_monitor.c） ---- */
void idle_reaper_start(void);
void idle_reaper_stop(void);
void perf_monitor_start(daemon_event_driver_t *driver);
void perf_monitor_stop(void);
uint64_t perf_now_us(void);
int64_t perf_slow_threshold_us(void);

/* ---- RPC 方法适配器（agent_d_rpc.c，main() 注册到 dispatcher） ---- */
void on_spawn_method(cJSON *params, int id, void *user_data);
void on_terminate_method(cJSON *params, int id, void *user_data);
void on_invoke_method(cJSON *params, int id, void *user_data);
void on_cancel_method(cJSON *params, int id, void *user_data);
void on_list_method(cJSON *params, int id, void *user_data);
void on_count_method(cJSON *params, int id, void *user_data);
void on_health_check_method(cJSON *params, int id, void *user_data);
void on_get_stats_method(cJSON *params, int id, void *user_data);

/* ---- run 引擎 RPC 适配器（agent_run_rpc.c，M1-1a 引擎下沉） ---- */
void on_run_method(cJSON *params, int id, void *user_data);
void on_run_cancel_method(cJSON *params, int id, void *user_data);
void on_run_stream_method(cJSON *params, int id, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_D_INTERNAL_H */
