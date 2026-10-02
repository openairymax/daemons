// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file agent_d_internal.h
 * @brief agent_d 翻译单元间的内部共享声明。
 *
 * 五件套化后（L3 生成器推广）：main.c / svc_agent_d.h / sources.cmake
 * 由 daemon_gen.py 从 .manifest 生成，daemon 配置与端点解析策略在
 * svc.c；RPC 方法入口（m_*）与生命周期钩子声明于生成头
 * svc_agent_d.h（L3 SSoT），此处只承载跨文件的内部符号。
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

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_AGENT_D_INTERNAL_H */
