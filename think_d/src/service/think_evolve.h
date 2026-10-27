/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file think_evolve.h
 * @brief 演化观测面采样：元认知快照 + memrpc evolve 计数上报。
 *
 * 0.1.20 W4：think_d 是唯一同时持有元认知状态（orch->metacognition）
 * 与 memrpc provider 的进程内采样点，故演化指标在此采样后经既有
 * monit_d observe_record_metric 通道推送，不新建导出面。
 */

#ifndef AIRY_THINK_EVOLVE_H
#define AIRY_THINK_EVOLVE_H

#include "think_service.h"

#include "daemon_event_driver.h"

#include <stdint.h>

#define THINK_EVOLVE_INTERVAL_MS 30000u

/* 单次采样快照：来源为策略侧 mc.stats JSON + provider no-op 计数 */
typedef struct {
    double patterns;
    double detections;
    double preemptions;
    double effectiveness;
    double records;
    double evolve_noop;
} think_evo_snapshot_t;

/**
 * @brief 将 mc.stats JSON 归一为标量快照（缺字段按 0 计）。
 *
 * @param mc_json [in] airy_mc_stats 输出（BORROW）。
 * @param noop [in] provider evolve no-op 累计计数。
 * @param out [out] 快照（TRANSFER - 调用方持有，函数整体覆写）。
 * @return 0 成功；-1 JSON 非法或参数为空。
 */
int think_evo_map(const char *mc_json, uint64_t noop, think_evo_snapshot_t *out);

/**
 * @brief 累计量差值（纯函数，不改基线）。
 *
 * obs 侧 counter 为累加语义，而源值为累计绝对值，故上行差值；源重启
 * 后 cur 小于基线时以 cur 为新基线，保证 /metrics 单调。
 *
 * @param cur [in] 本次累计值。
 * @param last [in] 上次基线。
 * @return 应上行的增量。
 */
double think_evo_delta(double cur, double last);

/**
 * @brief 挂载演化采样定时器（30s）。
 *
 * @param driver [in] 事件驱动（BORROW）。
 * @param svc [in] 已装配的 think 服务（BORROW - 生命周期长于定时器）。
 * @return 0 成功；-1 参数为空或定时器注册失败。
 */
int think_evolve_start(daemon_event_driver_t *driver, think_service_t *svc);

#endif /* AIRY_THINK_EVOLVE_H */
