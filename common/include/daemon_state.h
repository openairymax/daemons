/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file daemon_state.h
 * @brief Daemon 运行面形式化状态机（0.1.19 §3.7 N7，G38-2 前置）。
 *
 * 四态模型（G38-2「状态机穷举」判据）：每 daemon 运行态 ∈
 * {正常, 降级, 不可用, 恢复}。观测输入两路：
 *   - self_ok ：本体端到端可达（不变量 2，健康=可达而非启动快照）
 *   - deps_ok ：required 依赖全部可达（governance 面 daemon_dep 探测）
 * confirmed 为恢复确认窗策略位（由调用方按探测连击计算），机制零策略。
 *
 * 迁移全表（14 条，每条均有状态机测试，见 test_daemon_state.c）：
 *
 *   cur          | self_ok | deps_ok | confirmed | next
 *   -------------+---------+---------+-----------+--------------
 *   NOMINAL      |    1    |    1    |     -     | NOMINAL
 *   NOMINAL      |    1    |    0    |     -     | DEGRADED
 *   NOMINAL      |    0    |    -    |     -     | UNAVAILABLE
 *   DEGRADED     |    1    |    1    |     -     | NOMINAL
 *   DEGRADED     |    1    |    0    |     -     | DEGRADED
 *   DEGRADED     |    0    |    -    |     -     | UNAVAILABLE
 *   UNAVAILABLE  |    0    |    -    |     -     | UNAVAILABLE
 *   UNAVAILABLE  |    1    |    0    |     -     | DEGRADED
 *   UNAVAILABLE  |    1    |    1    |     0     | RECOVERING
 *   UNAVAILABLE  |    1    |    1    |     1     | NOMINAL
 *   RECOVERING   |    0    |    -    |     -     | UNAVAILABLE
 *   RECOVERING   |    1    |    0    |     -     | DEGRADED
 *   RECOVERING   |    1    |    1    |     0     | RECOVERING
 *   RECOVERING   |    1    |    1    |     1     | NOMINAL
 *
 * 非法状态输入一律折叠到 UNAVAILABLE（fail-closed）。deps_ok 仅在
 * self_ok 时有意义：本体不可达时依赖探测无从谈起，输入忽略。
 * llm_d 熔断三态（closed/open/half-open，证伪修正 H10）随 §3.8 A2
 * 落地后并入本图，不预置空壳。
 */

#ifndef AIRY_RT_DAEMON_STATE_H
#define AIRY_RT_DAEMON_STATE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** daemon 运行四态。 */
typedef enum dmn_state {
    DMN_ST_NOMINAL = 0,     /**< 正常：本体可达且依赖齐备 */
    DMN_ST_DEGRADED = 1,    /**< 降级：本体可达但 required 依赖缺失 */
    DMN_ST_UNAVAILABLE = 2, /**< 不可用：本体端到端不可达 */
    DMN_ST_RECOVERING = 3   /**< 恢复：曾不可用，探测复达未满确认窗 */
} dmn_state_t;

/**
 * @brief 状态机纯函数：按观测输入折叠出下一状态。
 *
 * @param cur       当前状态（非法值折叠到 DMN_ST_UNAVAILABLE）
 * @param self_ok   本体端到端可达
 * @param deps_ok   required 依赖全部可达（self_ok 为假时忽略）
 * @param confirmed 恢复确认窗已满足（恢复路径专用，调用方策略）
 * @return 下一状态
 */
dmn_state_t dmn_state_next(dmn_state_t cur, bool self_ok, bool deps_ok, bool confirmed);

/**
 * @brief 状态名（呈报/日志用）："nominal"/"degraded"/"unavailable"/
 *        "recovering"；非法值返回 "unavailable"（fail-closed 呈报）。
 */
const char *dmn_state_name(dmn_state_t st);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_DAEMON_STATE_H */
