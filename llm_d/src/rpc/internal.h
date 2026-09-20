/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file internal.h
 * @brief llm_d rpc 域（请求路径）内部声明。
 *
 * 由 llm_service_internal.h（253 行枢纽头，B16-S1 拆片）迁入：生成参数
 * 解析、provider 管理与复杂度评估三段。仅 service.c / service_request.c /
 * service_metrics.c / service_providers.c 及其单元测试消费；跨域禁止
 * include 本头。
 */

#ifndef AIRY_RT_LLM_RPC_INTERNAL_H
#define AIRY_RT_LLM_RPC_INTERNAL_H

#include "service.h"
#include "providers/core/registry.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Request-handling domain (service_request.c) ---- */

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

/* ---- Provider-management domain (service_providers.c) ---- */

void free_provider_configs(provider_config_t *providers, size_t count);
void merge_provider_configs(const provider_config_t *main_provs, size_t main_cnt,
                            const provider_config_t *user_provs, size_t user_cnt,
                            provider_config_t **out, size_t *out_cnt);
void register_router_endpoints(llm_service_t *svc);

/* ---- Complexity-evaluation and statistics domain (service_metrics.c) ---- */

/**
 * @brief Complexity assessment levels
 */
typedef enum {
    LLM_COMPLEXITY_SIMPLE = 0,
    LLM_COMPLEXITY_MODERATE = 1,
    LLM_COMPLEXITY_COMPLEX = 2
} llm_complexity_level_t;

llm_complexity_level_t assess_complexity(const char *input);
void log_routing_decision(const char *model, llm_complexity_level_t complexity,
                          size_t input_len, const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_RPC_INTERNAL_H */
