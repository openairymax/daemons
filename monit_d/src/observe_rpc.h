// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file observe_rpc.h
 * @brief 动态指标观测域 RPC 服务（0.1.9 M4：observe_d → monit_d 整编）。
 *
 * monit_d 内承载 observe 动态指标域：observe_rpc_init 初始化指标表并
 * 启动 Prometheus HTTP 端点（:9091），observe_rpc_register 在 monit 调度器
 * 登记 observe_* 方法，observe_rpc_cleanup 对称回收。/metrics 导出融合
 * 动态指标表与 unified-metrics 注册表（双 Prometheus 出口收敛，§5）。
 */

#ifndef AIRY_RT_MONIT_D_OBSERVE_RPC_H
#define AIRY_RT_MONIT_D_OBSERVE_RPC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化观测域（指标表 + 自监控预注册 + :9091 HTTP 端点）。返回 0 成功。 */
int observe_rpc_init(void);

/* 对称清理（幂等）。 */
void observe_rpc_cleanup(void);

/* 在 monit_d 方法调度器上登记 observe_* 方法（3 个）。 */
void observe_rpc_register(void *disp);

/* ── 内部 API（供单元测试直接驱动指标表与 HTTP 导出） ─────────────── */

typedef enum { OBS_GAUGE = 0, OBS_COUNTER = 1 } obs_metric_type_t;

/* 记录一条动态指标：counter 累加、gauge 覆盖；unit 为 NULL 时保持原值。 */
int obs_rpc_record(const char *name, double value, const char *unit, obs_metric_type_t type);

/* 将动态指标表格式化为 Prometheus 文本，返回写入长度（<0 失败）。 */
int obs_rpc_format(char *buffer, size_t buffer_size);

/* 当前动态指标条数。 */
size_t obs_rpc_metric_count(void);

/* 处理 HTTP 抓取请求：GET /metrics 命中时输出融合导出（动态表 + um 注册表）
 * 的完整 HTTP 响应（调用方 AIRY_FREE），返回 0；非抓取请求返回 -1。 */
int obs_rpc_handle_http(const char *request, size_t request_len, char **response,
                        size_t *response_len);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_MONIT_D_OBSERVE_RPC_H */
