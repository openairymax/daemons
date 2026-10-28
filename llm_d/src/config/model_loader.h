/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file model_loader.h
 * @brief llm_d config 域·模型配置装载面。
 *
 * 自 config_internal.h 按关注点切分（A4 入度 ≤5）：只承载 provider 配置的
 * 装载入口（YAML/JSON 两路统一到同一结果类型），与定价规则面
 * （pricing_api.h）解耦。
 */

#ifndef AIRY_RT_LLM_MODEL_LOADER_H
#define AIRY_RT_LLM_MODEL_LOADER_H

#include "providers/core/provider_config.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int svc_load_model_config(const char *config_path, provider_config_t **out_providers,
                          size_t *out_count);
int svc_load_model_config_json(const char *config_path, provider_config_t **out_providers,
                               size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* AIRY_RT_LLM_MODEL_LOADER_H */
