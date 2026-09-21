/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file internal.h
 * @brief llm_d config 域（模型配置装载）内部声明。
 *
 * 由 llm_service_internal.h（253 行枢纽头，B16-S1 拆片）迁入：YAML/JSON
 * 双格式的定价规则与模型配置装载入口。仅 service_config*.c 与 service.c
 * 消费；跨域禁止 include 本头。YAML 解析结果类型见 config/types.h。
 */

#ifndef AIRY_RT_LLM_CONFIG_INTERNAL_H
#define AIRY_RT_LLM_CONFIG_INTERNAL_H

#include "cost_tracker.h"
#include "providers/core/registry.h"

#include <cjson/cJSON.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Config-loading domain (service_config.c) ---- */

int ends_with(const char *str, const char *suffix);
pricing_rule_t *load_pricing_rules(cJSON *root, int *count);
void free_pricing_rules(pricing_rule_t *rules, int count);
int load_pricing_rules_from_yaml(const char *config_path, pricing_rule_t **out_rules,
                                 int *out_count);
int svc_load_model_config(const char *config_path, provider_config_t **out_providers,
                          size_t *out_count);
int svc_load_model_config_json(const char *config_path, provider_config_t **out_providers,
                               size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_CONFIG_INTERNAL_H */
