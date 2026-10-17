// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file info_rpc.h
 * @brief 系统信息域 RPC 服务（0.1.9 M4：info_d → monit_d 整编）。
 *
 * monit_d 内承载系统信息采集域：info_rpc_init 初始化状态并完成首采，
 * info_rpc_start 启动周期采集线程（5s 快照、64 深度环形历史），
 * info_rpc_register 登记 info_* 方法，info_rpc_cleanup 停止线程并回收。
 * gateway info.* cap 改路由到 monit 命名空间。
 */

#ifndef AIRY_RT_MONIT_D_INFO_RPC_H
#define AIRY_RT_MONIT_D_INFO_RPC_H

#include <cjson/cJSON.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化系统信息域状态（含首采快照，不启动线程）。返回 0 成功。 */
int info_rpc_init(void);

/* 启动周期采集线程（须在 info_rpc_init 之后；幂等）。返回 0 成功。 */
int info_rpc_start(void);

/* 对称清理（停止采集线程，幂等）。 */
void info_rpc_cleanup(void);

/* 在 monit_d 方法调度器上登记 info_* 方法（4 个）。 */
void info_rpc_register(void *disp);

/* ── 内部 API（供单元测试直接驱动采集与历史环） ───────────────────── */

typedef struct {
    double cpu_usage_pct;
    uint64_t total_memory_kb;
    uint64_t free_memory_kb;
    uint64_t used_memory_kb;
    double memory_usage_pct;
    uint64_t disk_total_kb;
    uint64_t disk_free_kb;
    uint64_t disk_used_kb;
    double disk_usage_pct;
    int cpu_cores;
    uint64_t uptime_sec;
    uint64_t timestamp;
} info_snapshot_t;

/* 执行一次系统信息采集，填充快照。返回 0 成功。 */
int info_rpc_collect(info_snapshot_t *snap);

/* 追加一条历史快照（环形覆盖）。 */
void info_rpc_hist_add(const info_snapshot_t *snap);

/* 取最近 limit 条历史的 JSON 数组（时间序）；limit 钳到 [0, 64]。 */
cJSON *info_rpc_hist_json(int limit);

/* 由快照构建 JSON 对象（system/history 响应共用）。 */
cJSON *info_rpc_snap_json(const info_snapshot_t *snap);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_MONIT_D_INFO_RPC_H */
