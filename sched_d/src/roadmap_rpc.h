/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file roadmap_rpc.h
 * @brief Roadmap scheduler RPC binding for sched_d (roadmap.plan/absorb/
 *        cancel/replan/stats).
 *
 * 蓝图调度接线（2026-08-25 修复）：此前 "新蓝图调度"（roadmap_sched
 * 三级路由：L1 状态机 -> L2 语义缓存 -> L3 全量规划）只在 CLI 与 work_hall
 * 内运转，sched_d 未接入——上层经 sched.* 命名空间调用 plan/absorb 会得到
 * method not found。本模块在 sched_d 内创建 airy_roadmap_sched_t 实例并
 * 注册 roadmap.* 方法族，使 HTTP/JSON-RPC 客户端可经 sched_d 使用蓝图
 * 调度能力（L2 语义缓存独立持久化于 $AIRY_DATA_DIR/agentrt/roadmap/）。
 */

#ifndef AIRY_RT_SCHED_ROADMAP_RPC_H
#define AIRY_RT_SCHED_ROADMAP_RPC_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 roadmap 调度器（创建 L2 语义缓存实例）。
 * @return 0 成功；非 0 失败（实例不可用，plan/absorb 返回错误）
 */
int roadmap_rpc_init(void);

/** @brief 销毁 roadmap 调度器实例。 */
void roadmap_rpc_cleanup(void);

/**
 * @brief 注册 roadmap.* RPC 方法到调度器分发器。
 * @param disp 事件驱动分发器（method_dispatcher_t*）
 */
void roadmap_rpc_register(void *disp);

/** @brief 实例是否可用。 */
int roadmap_rpc_ready(void);

/* 事件驱动回调包装（供 main.c 经 method_dispatcher_register 注册）：
 * user_data 携带客户端 socket fd。 */
void on_roadmap_plan_method(cJSON *params, int id, void *user_data);
void on_roadmap_absorb_method(cJSON *params, int id, void *user_data);
void on_roadmap_cancel_method(cJSON *params, int id, void *user_data);
void on_roadmap_replan_method(cJSON *params, int id, void *user_data);
void on_roadmap_stats_method(cJSON *params, int id, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_SCHED_ROADMAP_RPC_H */
