/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file prov_internal.h
 * @brief llm_d rpc 域·provider 装配面。
 *
 * 自 rpc_internal.h 按关注点切分（A4 入度 ≤5）：只承载 provider 配置的释放、
 * 合并与端点注册，与生成参数解析面（gen_params.h）解耦。
 */

#ifndef AIRY_RT_LLM_PROV_INTERNAL_H
#define AIRY_RT_LLM_PROV_INTERNAL_H

#include "service.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void free_provider_configs(provider_config_t *providers, size_t count);
void merge_provider_configs(const provider_config_t *main_provs, size_t main_cnt,
                            const provider_config_t *user_provs, size_t user_cnt,
                            provider_config_t **out, size_t *out_cnt);
void register_router_endpoints(llm_service_t *svc);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_PROV_INTERNAL_H */
