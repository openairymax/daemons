/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file pricing_api.h
 * @brief llm_d config 域·定价规则装载面。
 *
 * 自 config_internal.h 按关注点切分（A4 入度 ≤5）：只承载定价规则的装载与
 * 释放，与模型配置装载（model_loader.h）解耦。装配面（rpc/）可消费，config
 * 域之外不得反向 include 本头。
 */

#ifndef AIRY_RT_LLM_PRICING_API_H
#define AIRY_RT_LLM_PRICING_API_H

#include "cost_tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

void free_pricing_rules(pricing_rule_t *rules, int count);
int load_pricing_rules_from_yaml(const char *config_path, pricing_rule_t **out_rules,
                                 int *out_count);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_PRICING_API_H */
