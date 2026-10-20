/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_config.h: Provider 域配置装载结果契约（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_CONFIG_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    /* 线格式名（model.yaml api_format），适配器查找键；NULL = 未声明，
     * 由装配面回落行协议适配。 */
    const char *api_format;
    const char *api_key;
    const char *api_base;
    const char *organization;
    /* 端点附加头（'...; ...' 或换行分隔），随配置注入请求头链。 */
    const char *extra_headers;
    double timeout_sec;
    int max_retries;
    char **models;
    /* 与 models 同下标对齐的每模型输出上限（model.yaml max_output 的 token
     * 数，0 = 未配置）。NULL 表示调用方未提供容量表。生成参数解析靠它兑现
     * "配置了上限就按下限截断"。 */
    const int *model_max_output;
} provider_config_t;

typedef struct service_config {
    size_t llm_cache_capacity;
    uint32_t llm_cache_ttl_sec;
    int max_retries;
    uint32_t timeout_ms;
    const char *token_encoding;
    provider_config_t *providers;
    size_t provider_count;
} service_config_t;

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_CONFIG_H */
