/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file router_state.h
 * @brief 端点健康面的落盘与恢复（0.1.20 D2-3③）。
 *
 * 只持久化重启后仍有意义的量：连续失败计数、失败距今时长、熔断状态
 * 与剩余保持期。不存绝对截止时刻——airy_time_ms() 是单调时钟，重启后
 * 归零，存绝对值会让端点凭空 OPEN 数十天。保存侧的时长换算在
 * router_state.c 内完成，本头只暴露三个动作。
 *
 * 生命周期：router_state_load() 在 llm_router_init() 末尾读入恢复快照；
 * llm_router_register_endpoint() 逐个消费匹配项；ep_note() 在健康面
 * 发生变化时改写文件；llm_router_destroy() 收尾再写一次。
 */

#ifndef AIRY_RT_LLM_ROUTER_STATE_H
#define AIRY_RT_LLM_ROUTER_STATE_H

#include "router/core/router_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 读入恢复快照。文件缺失或损坏时静默跳过，不阻断路由初始化。 */
void router_state_load(void);

/** 把当前端点健康面写入快照数组（保留重注册前的状态，供热重载复用）。
 * 调用方须持 ctx->mutex。 */
void rtr_state_capture(void);

/** 按 provider/model 把快照回填到新注册端点，命中即从快照移除。
 * 调用方须持 ctx->mutex。 */
void rtr_ep_restore(llm_endpoint_t *ep);

/** 把当前端点健康面落盘。端点表为空时不写文件（保留上次内容，避免
 * 热重载的短暂空窗把状态抹掉）。调用方须持 ctx->mutex。 */
int rtr_state_save(void);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_ROUTER_STATE_H */
