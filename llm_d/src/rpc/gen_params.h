/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file gen_params.h
 * @brief llm_d rpc 域·生成参数解析面。
 *
 * 自 rpc_internal.h 按关注点切分（A4 入度 ≤5）：只承载同步/流式两路共用的
 * 生成参数唯一解析点，与 provider 管理面（prov_internal.h）解耦。
 */

#ifndef AIRY_RT_LLM_GEN_PARAMS_H
#define AIRY_RT_LLM_GEN_PARAMS_H

#include "service.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 生成参数唯一解析点：模型名与输出上限的三方来源收敛。
 *
 * 三方来源：调用方显式值（意图）> 注册表中该模型声明的上限（边界）>
 * 引擎默认上限（兜底）。同步与流式两条路径共用，避免口径漂移。
 *
 * @param svc          服务实例（取 default_model / default_max_output_tokens）
 * @param prov         选定的 provider（取每模型上限，可为 NULL）
 * @param manager      调用方请求配置
 * @param out_model    输出模型名缓冲（非 NULL）
 * @param model_size   out_model 容量
 * @param out_max_tokens 输出解析后的 max_tokens（0 = 不设，交上游默认）
 */
void resolve_gen_params(const llm_service_t *svc, const provider_t *prov,
                        const llm_request_config_t *manager, char *out_model, size_t model_size,
                        int *out_max_tokens);

/* 生效输出上限的来源：截断告警据此给出可行动的处置建议。 */
typedef enum {
    GEN_BOUND_NONE = 0, /* 未设上限（0，交上游默认） */
    GEN_BOUND_REQ,      /* 调用方显式意图 */
    GEN_BOUND_MODEL     /* 模型声明上限或引擎默认 */
} gen_bound_src_t;

/**
 * @brief 归因生效上限（纯函数，便于单测）：显式值 >0 且未被边界改写
 *        （resolved == explicit）归调用方；resolved <= 0 表示未设上限；
 *        其余归模型声明上限或引擎默认。
 *
 * @param explicit_max 调用方请求的 max_tokens（<=0 表示未声明）
 * @param resolved     resolve_gen_params 实际下传的 max_tokens
 */
gen_bound_src_t gen_bound_src(int explicit_max, int resolved);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_GEN_PARAMS_H */
