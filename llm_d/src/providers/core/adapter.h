/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/**
 * @file adapter.h
 * @brief Provider 适配层契约（B16-S3 拆分自 provider.h）。
 *
 * 适配器实现本契约，以 provider_t 条目接入 registry（装配 SSoT 归
 * adapters/adapter_table.c）。B16-S3.3 兑现方案 12.16.3 五槽演进：
 * complete / complete_stream 两整请求入口由适配器以两行 shim 交
 * core/adapter.c 通用 driver 编排（密钥刷新、出网、错误归一、流末装配
 * 的唯一实现），适配层仅剩五纯函数槽 + 数据槽——纯函数、零 I/O、
 * 表驱动。机制层（transport.h）禁止适配器自持副本。
 */

#ifndef LLM_D_PROVIDERS_CORE_ADAPTER_H
#define LLM_D_PROVIDERS_CORE_ADAPTER_H

#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct provider_ctx provider_ctx_t;

typedef struct {
    /* ── 身份数据槽 ── */
    const char *name;
    const char *tag; /* 日志前缀（如 "OPENAI"），driver 统一日志使用 */
    const char *default_model;
    const char *default_base_url;
    const char *chat_path;                 /* 追加在 api_base 之后的端点路径 */
    const provider_header_spec_t *headers; /* 鉴权与静态附加头声明；NULL = 无鉴权 */

    /* ── 生命周期槽 ── */
    provider_ctx_t *(*init)(const char *name, const char *api_key, const char *api_base,
                            const char *organization, double timeout_sec, int max_retries);
    void (*destroy)(provider_ctx_t *ctx);

    /* ── 限流槽：返回 ctx 内嵌的令牌桶（无则 NULL）。非流式经此接入 core
     * 限流出网（RPM + 429 退避）；流式一律直连（限流语义不适用增量流）。 */
    struct provider_rate_limiter *(*rate_limiter)(provider_ctx_t *ctx);

    /* ── 四槽入口：适配器以两行 shim 交 core driver 通用实现
     *（provider_driver_complete / provider_driver_complete_stream）。 */
    int (*complete)(provider_ctx_t *ctx, const llm_request_config_t *manager,
                    llm_response_t **out_response);
    int (*complete_stream)(provider_ctx_t *ctx, const llm_request_config_t *manager,
                           llm_stream_callback_t callback, void *callback_data,
                           llm_response_t **out_response);

    /* ── 五纯函数槽（方案 12.16.3）：适配层仅存的协议差异面。
     * feed_event 为 NULL 表示 OpenAI 行协议流（on_chunk 走 core
     * provider_openai_on_chunk 机制件）；非 NULL 为具名事件流。 */
    char *(*build_request)(const llm_request_config_t *manager);
    int (*parse_response)(const char *body, llm_response_t **out);
    provider_sse_event_cb_t feed_event;
} provider_adapter_t;

/* ── 通用 completion driver（B16-S3.3，core/adapter.c 唯一实现）──
 * 两家（乃至未来所有）适配器的四槽入口编排件：密钥刷新 → build_request
 *（流式置 stream=1）→ provider_http_exec 出网 → 错误归一 → parse_response /
 * 流末 take+flush 装配 → 统一日志。适配器以两行 shim 传入自身 ops 调用，
 * core 不持有任何厂商符号。 */
int provider_driver_complete(provider_ctx_t *ctx, const provider_adapter_t *ops,
                             const llm_request_config_t *manager,
                             llm_response_t **out_response);
int provider_driver_complete_stream(provider_ctx_t *ctx, const provider_adapter_t *ops,
                                    const llm_request_config_t *manager,
                                    llm_stream_callback_t callback, void *callback_data,
                                    llm_response_t **out_response);

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

/**
 * @brief 按厂商名查适配器 ops（装配 SSoT 在 adapters/adapter_table.c）。
 *
 * core 侧不持有任何厂商符号与厂商名分支；注册新适配仅在 adapter_table.c
 * 表尾加一行。未知名回落 OpenAI 兼容适配：任一非内建厂商（glm / qwen /
 * moonshot / siliconflow / spark / minimax / 自定义名）仅需 model.yaml 提
 * 供 base_url + api_key 即以统一 OpenAI Chat Completions 协议接入。
 *
 * @param name 厂商名（可为 NULL，等价未知名）
 * @return 适配器 ops（永不为 NULL）
 */
const provider_adapter_t *provider_adapter_lookup(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_ADAPTER_H */
