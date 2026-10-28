/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file prov_internal.h
 * @brief llm_d rpc 域·provider 配置装配面。
 *
 * 自 rpc_internal.h 按关注点切分（A4 入度 ≤5）：只承载 provider 配置的
 * 释放与合并；端点注册归 service.c（持有服务实例的域）。
 */

#ifndef AIRY_RT_LLM_PROV_INTERNAL_H
#define AIRY_RT_LLM_PROV_INTERNAL_H

#include "providers/core/provider_config.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void free_provider_configs(provider_config_t *providers, size_t count);
void merge_provider_configs(const provider_config_t *main_provs, size_t main_cnt,
                            const provider_config_t *user_provs, size_t user_cnt,
                            provider_config_t **out, size_t *out_cnt);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_PROV_INTERNAL_H */
