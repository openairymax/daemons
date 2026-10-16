// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file local.c
 * @brief Local-model adapter（OpenAI 兼容协议，无鉴权本地端点）。
 *
 * W2 收口：本件自持的完整编排（出网、错误归一、流末装配）
 * 的逐字重复实现收敛归 B16-S3.3 五槽契约——四槽入口以两行 shim 交
 * core/adapter.c 通用 driver 唯一编排；请求装配/响应解析复用
 * core/envelope.c 的 OpenAI 协议件（闭 LOCAL_DEFAULT_MODEL 参后交纯函数槽）。
 * 适配层只留厂商差异面（默认端点/模型、120s 超时、无鉴权头声明、
 * 日志 tag），不含任何 curl_* 调用。
 *
 * 无鉴权是设计使然：LOCAL_HEADERS 声明 PROVIDER_AUTH_NONE，core 头链
 * （http.c provider_headers_build）对 NONE 不拼 Authorization，故 driver
 * 统一的密钥热重载对本地端点无功能影响——key 永不外发。
 */

#include "airy_memory.h"
#include "error.h"
#include "daemon_platform_ext.h"
#include "providers/providers_internal.h"
#include "svc_logger.h"

/* 代码级默认端点：仅当调用方未传 base_url 时使用。部署标准配置
 * （model.yaml local provider / llm: 段）显式传 base_url —— Ollama 默认
 * http://localhost:11434，vLLM 走 OpenAI 兼容格式 8080/v1——实际以
 * yaml 传入值为准（2026-08-20 审查：代码默认与 yaml 默认的差异为
 * 文档级，非功能缺陷）。 */
#define LOCAL_DEFAULT_BASE "http://localhost:8080/v1"
#define LOCAL_DEFAULT_MODEL "gpt-3.5-turbo"
#define LOCAL_DEFAULT_TIMEOUT 120.0
#define LOCAL_CHAT_PATH "/chat/completions"

/* 请求头声明：本地端点无鉴权（设计使然），core 仅附加 Content-Type。 */
static const provider_header_spec_t LOCAL_HEADERS = {.auth = PROVIDER_AUTH_NONE};

typedef struct {
    provider_base_ctx_t base;
} local_ctx_t;

extern const provider_adapter_t local_ops; /* shim 自引用 ops */

static provider_ctx_t *local_init(const char *name, const char *api_key, const char *api_base,
                                  const char *organization, double timeout_sec, int max_retries)
{
    local_ctx_t *ctx = (local_ctx_t *)AIRY_CALLOC(1, sizeof(local_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: LOCAL: INIT-FAIL — OOM allocating ctx (size=%zu)",
                      sizeof(local_ctx_t));
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    double timeout = timeout_sec > 0 ? timeout_sec : LOCAL_DEFAULT_TIMEOUT;
    provider_base_init(&ctx->base, name, NULL, api_base, NULL, timeout, max_retries,
                       LOCAL_DEFAULT_BASE);

    /* 本地慢推理是预期行为而非瞬时故障：超时不做重发（retry_class =
     * CONNECT_ONLY，仅连接建立失败可重试），故额外重试预算在全局默认上降一次；
     * 首连失败仍可重发一次，避免端点刚起未就绪时直接失败。 */
    if (ctx->base.max_retries > 1)
        ctx->base.max_retries--;
    ctx->base.retry_class = PROVIDER_RETRY_CONNECT_ONLY;

    SVC_LOG_INFO("C-L02: LOCAL: INIT api_base=%s model=%s timeout=%.1fs max_retries=%d "
                 "has_api_key=%d (no auth, local endpoint, higher default timeout)",
                 ctx->base.api_base, LOCAL_DEFAULT_MODEL, ctx->base.timeout_sec,
                 ctx->base.max_retries, 0);

    return (provider_ctx_t *)ctx;
}

static void local_destroy(provider_ctx_t *ctx_ptr)
{
    if (ctx_ptr) {
        SVC_LOG_DEBUG("C-L02: LOCAL: DESTROY ctx=%p", (void *)ctx_ptr);
        AIRY_FREE(ctx_ptr);
    }
}

/* 纯函数槽：OpenAI 请求装配为 core 共享协议件（envelope.c），此处仅闭
 * LOCAL_DEFAULT_MODEL 参；响应解析直接挂 provider_parse_openai_response。
 *
 * Ollama 专有扩展（D3）：keep_alive 为 Ollama 顶层扩展字段（OpenAI 兼容
 * 端点受支持），默认 5min 卸载导致「间隔后首请求」冷加载尖峰，此处置
 * "30m" 摊薄重载。num_ctx 不在 OpenAI 兼容端点生效（官方文档明确该端点
 * 不提供上下文大小设置），num_predict 已由顶层 max_tokens 覆盖，故均不注入。
 * 常量置本件（厂商差异面），不新增用户配置面。 */
#define LOCAL_EXTRA_PARAMS "{\"keep_alive\":\"30m\"}"

static char *local_build_request(const llm_request_config_t *manager)
{
    return provider_build_openai_request(manager, LOCAL_DEFAULT_MODEL, LOCAL_EXTRA_PARAMS);
}

static int local_complete(provider_ctx_t *ctx, const llm_request_config_t *manager,
                          llm_response_t **out_response)
{
    return provider_driver_complete(ctx, &local_ops, manager, out_response);
}

static int local_complete_stream(provider_ctx_t *ctx, const llm_request_config_t *manager,
                                 llm_stream_callback_t callback, void *callback_data,
                                 llm_response_t **out_response)
{
    return provider_driver_complete_stream(ctx, &local_ops, manager, callback, callback_data,
                                           out_response);
}

/* 五槽装配：feed_event 留空（NULL）= OpenAI 行协议流，on_chunk 机制件由
 * driver 挂 core provider_openai_on_chunk；本地端点无令牌桶（rate_limiter
 * 留空，driver 判 NULL 即不接限流出网）。 */
const provider_adapter_t local_ops = {
    .name = "local", .tag = "LOCAL",
    .default_model = LOCAL_DEFAULT_MODEL, .default_base_url = LOCAL_DEFAULT_BASE,
    .chat_path = LOCAL_CHAT_PATH, .headers = &LOCAL_HEADERS,
    .init = local_init, .destroy = local_destroy,
    .complete = local_complete, .complete_stream = local_complete_stream,
    .build_request = local_build_request, .parse_response = provider_parse_openai_response,
};