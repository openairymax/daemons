/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file adapter.h
 * @brief Provider 适配层契约（B16-S3 拆分自 provider.h）。
 *
 * 适配器实现本契约，以 provider_t 条目接入 registry（c7 起装配 SSoT 归
 * adapters/adapter_table.c）。槽位现状为过渡形态（complete / complete_stream
 * 整请求入口），c4/c5 SSE 与 toolstream 下沉后演进为方案 12.16.3 的
 * build_request / parse_response / feed_event / map_usage / map_tool_delta
 * 五纯函数槽。适配器禁止自持机制层（transport.h）任何副本。
 */

#ifndef LLM_D_PROVIDERS_CORE_ADAPTER_H
#define LLM_D_PROVIDERS_CORE_ADAPTER_H

#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct provider_ctx provider_ctx_t;

typedef struct {
    const char *name;
    const char *default_model;
    const char *default_base_url;
    provider_ctx_t *(*init)(const char *name, const char *api_key, const char *api_base,
                            const char *organization, double timeout_sec, int max_retries);
    void (*destroy)(provider_ctx_t *ctx);
    int (*complete)(provider_ctx_t *ctx, const llm_request_config_t *manager,
                    llm_response_t **out_response);
    int (*complete_stream)(provider_ctx_t *ctx, const llm_request_config_t *manager,
                           llm_stream_callback_t callback, void *callback_data,
                           llm_response_t **out_response);
} provider_adapter_t;

typedef struct {
    const char *name;
    const provider_adapter_t *adapter;
    provider_ctx_t *ctx;
    char **models;
    /* 与 models 同下标对齐的每模型输出上限（0 = 未配置）：registry 装配时
     * 从 provider_config_t 复制，供生成参数解析查询。 */
    int *model_max_output;
} provider_t;

/* 获取 provider 的 base 上下文（api_base/api_key/timeout_sec 等）。
 * 约定：所有 provider 的 ctx 首字段均为 provider_base_ctx_t base。 */
provider_base_ctx_t *provider_base_ctx(provider_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_ADAPTER_H */
