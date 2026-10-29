/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_rtr_health.h
 * @brief 上游整体健康聚合报告面（0.1.20 D2-5）。
 *
 * 端点级健康（llm_endpoint_t 与 llm_ep_* 迁移）归 router/core/llm_router.h；
 * 本头只承载「把端点状态聚合成一个上游结论」的判据与 wire 拼写。
 * 二者分头是因为消费方不同：health_check 与探测回填只需聚合结论，不必
 * 为此摘入选路面、测量面与统计面（0.1.20 §7.5 扇入约束）。聚合本身不
 * 消费 llm_endpoint_t，故拆出不产生「一次判定摘两件」的代价。
 *
 * @owner team-A
 */

#ifndef AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_RTR_HEALTH_H
#define AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_RTR_HEALTH_H

#include "airy_abi.h"

#include <stddef.h>

AIRY_ABI_BEGIN

/**
 * @brief 上游整体健康三态（0.1.20 D2-5）。
 *
 * 进程存活与上游可用是两件事：前者由 IPC 应答证明，后者只能由端到端的
 * 探测/请求结果证明。health_check 报的是后者，故状态必须从端点健康面
 * 聚合得出，不得以服务句柄非空冒充。
 */
typedef enum {
    LLM_HEALTH_DOWN = 0,     /**< 无可用上游（全部熔断或未注册） */
    LLM_HEALTH_DEGRADED = 1, /**< 部分上游不可用，仍可服务 */
    LLM_HEALTH_OK = 2        /**< 全部启用端点 CLOSED */
} llm_health_state_t;

/** 聚合快照：total 为启用端点数，ok 为 CLOSED 数，down 为 OPEN/HALF 数。 */
typedef struct {
    llm_health_state_t state;
    size_t total;
    size_t ok;
    size_t down;
} llm_health_snap_t;

/**
 * @brief 锁内聚合端点健康面。
 * @param out 可为 NULL；非 NULL 时回填计数明细。
 * @return 聚合状态（router 未初始化或无启用端点时 DOWN）。
 */
llm_health_state_t llm_rtr_health(llm_health_snap_t *out);

/** 三态名的 wire 拼写（"ok"/"degraded"/"down"），用于 health_check 应答。 */
const char *llm_health_name(llm_health_state_t state);

/**
 * @brief 按 provider 记账一次端到端可达性观测（0.1.20 D2-5）。
 *
 * 主动探测只到 provider 粒度（/models + 深度探针），而健康面按
 * provider×model 记账：同 provider 的全部端点共享同一条上游链路，故一次
 * 观测对全部同名端点生效。失败按 AIRY_ERR_IO 记账——探测失败是传输面
 * 不可达，与调用期连接失败同属可切换类（llm_fo_reason 显式表）。
 *
 * @param provider 端点 provider 名。
 * @param ok       1 可达，0 不可达。
 * @return 命中的端点条数（0 表示 provider 未注册或 router 未初始化）。
 */
size_t llm_rtr_prov_note(const char *provider, int ok);

AIRY_ABI_END

#endif /* AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_RTR_HEALTH_H */
