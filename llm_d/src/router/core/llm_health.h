/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file llm_health.h
 * @brief 端点健康面：连续失败计数与熔断三态（状态迁移件 + 锁内包装）。
 *
 * 依据 0.1.19 架构改进方案 §3.8.2 第 1 条。llm_endpoint_t 的四个健康字段
 * 只由本件迁移，使「端点还能不能用」成为一处可判定、可单测的规则：
 *
 *   CLOSED --连续失败达 LLM_EP_FAIL_MAX，或 AUTH 一次--> OPEN
 *   OPEN   --open_until_ms 过期--> HALF，只放行单次探测
 *   HALF   --探测成功--> CLOSED；探测失败--> OPEN
 *
 * 三条口径：
 *   1. 只有 llm_fo_reason() 认定的可切换类错误才计入健康面。请求本身非法
 *      或策略拒绝不代表端点故障，恒不计，否则 BAD_REQUEST 会熔断健康端点。
 *   2. AUTH 一次即开：密钥错误逐请求重撞只会拖长故障面，须立刻摘除端点。
 *   3. 半开探测靠 probe_until_ms 占位：探测方崩溃未回报时槽位按时释放，
 *      避免端点永久卡在 HALF。
 *
 * 时间字段为 32 位毫秒，`(int32_t)(截止 - 当前) > 0` 即未到期，可安全跨越
 * 2^32 回绕；32 位单字段读写天然原子，选择路径无需加锁即可读状态。
 * 写路径持 router 全局 mutex，读路径最坏看到过渡态，方向安全。
 */

#ifndef AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_HEALTH_H
#define AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_HEALTH_H

#include "airy_abi.h"
#include "router/core/llm_router.h"

AIRY_ABI_BEGIN

/** 熔断三态。 */
typedef enum {
    LLM_EP_CLOSED = 0,
    LLM_EP_OPEN = 1,
    LLM_EP_HALF = 2
} llm_ep_state_t;

/** 连续失败阈值：达此值转 OPEN。 */
#define LLM_EP_FAIL_MAX 3U

/** OPEN 保持时长（毫秒）。 */
#define LLM_EP_OPEN_MS 30000U

/** 半开探测最长占用（毫秒）：探测方未回报则释放槽位，允许重新探测。 */
#define LLM_EP_PROBE_MS 10000U

/**
 * 由健康字段推导当前状态。
 * @param ep  端点。
 * @param now airy_time_ms() 截断到 32 位。
 */
llm_ep_state_t llm_ep_state(const llm_endpoint_t *ep, uint32_t now);

/**
 * 判定本次调用是否放行；HALF 状态下首个调用方占用探测槽。
 * @return 1 放行，0 拒绝（OPEN 期或探测槽被占）。
 */
int llm_ep_admit(llm_endpoint_t *ep, uint32_t now);

/** 记录一次失败，按判据迁移状态。不可切换类错误不计入。 */
void llm_ep_fail(llm_endpoint_t *ep, int err, uint32_t now);

/** 记录一次成功：清零计数并归 CLOSED。 */
void llm_ep_ok(llm_endpoint_t *ep);

/**
 * 锁内按 provider/model 定位端点的包装（调用侧使用）。
 * 端点未注册或 router 未初始化时一律按拒绝方向处理。
 */
int llm_rtr_admit(const char *provider, const char *model);

/** 锁内记录失败。@return 0 已记录，负值未找到端点。 */
int llm_rtr_fail(const char *provider, const char *model, int err);

/** 锁内记录成功。@return 0 已记录，负值未找到端点。 */
int llm_rtr_ok(const char *provider, const char *model);

AIRY_ABI_END

#endif /* AIRY_RT_DAEMON_LLM_D_ROUTER_LLM_HEALTH_H */
