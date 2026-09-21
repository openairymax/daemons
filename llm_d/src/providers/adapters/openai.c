// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file openai.c
 * @brief OpenAI adapter（B16-S3.3）：五槽契约下的厂商差异面——默认端点/模型、
 * Bearer 头声明、令牌桶限流（RPM/TPM + 429 退避为 OpenAI 云端专属）。
 * 请求装配与响应解析复用 core/envelope.c 的 OpenAI 协议件（闭 default_model
 * 参后交纯函数槽）；编排归 core/adapter.c driver，本文件不含任何完成流程。
 *
 * 对外公共符号 openai_ops 不变。
 */

#include "airy_memory.h"
#include "error.h"
#include "daemon_platform_ext.h"
#include "core/adapter.h"
#include "core/rate_limit.h"
#include "svc_logger.h"

#define OPENAI_DEFAULT_BASE "https://api.openai.com/v1"
#define OPENAI_DEFAULT_MODEL "gpt-3.5-turbo"
#define OPENAI_CHAT_PATH "/chat/completions"

/* 请求头声明：Bearer 鉴权（core 按 R-1 省略空凭据），无厂商附加头。 */
static const provider_header_spec_t OPENAI_HEADERS = {.auth = PROVIDER_AUTH_BEARER};

/* 运行时上下文：首字段必须为 provider_base_ctx_t（provider_base_ctx()
 * 依赖此布局）。令牌桶限流为 OpenAI 云端专属（RPM/TPM + 429 退避）。 */
typedef struct {
    provider_base_ctx_t base;
    provider_rate_limiter_t rl;
} openai_ctx_t;

extern const provider_adapter_t openai_ops; /* shim 自引用 ops */

static provider_ctx_t *openai_init(const char *name, const char *api_key, const char *api_base,
                                   const char *organization, double timeout_sec, int max_retries)
{
    openai_ctx_t *ctx = (openai_ctx_t *)AIRY_CALLOC(1, sizeof(openai_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: OPENAI: INIT-FAIL reason=oom STACK: openai_init");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    provider_base_init(&ctx->base, name, api_key, api_base, organization, timeout_sec, max_retries,
                       OPENAI_DEFAULT_BASE);

    provider_rl_init(&ctx->rl);

    airy_random_init();

    SVC_LOG_INFO("C-L02: OPENAI: INIT api_base=%s timeout=%.1fs retries=%d has_api_key=%d "
                 "RPM=%d TPM=%ld",
                 ctx->base.api_base[0] ? ctx->base.api_base : OPENAI_DEFAULT_BASE,
                 ctx->base.timeout_sec, ctx->base.max_retries, ctx->base.api_key[0] ? 1 : 0,
                 PROVIDER_RL_DEFAULT_RPM, (long)PROVIDER_RL_DEFAULT_TPM);

    return (provider_ctx_t *)ctx;
}

static void openai_destroy(provider_ctx_t *ctx_ptr)
{
    if (!ctx_ptr)
        return;
    openai_ctx_t *ctx = (openai_ctx_t *)ctx_ptr;
    SVC_LOG_INFO("C-L02: OPENAI: DESTROY ctx=%p", (void *)ctx_ptr);
    provider_rl_destroy(&ctx->rl);
    AIRY_FREE(ctx_ptr);
}

/* 纯函数槽：OpenAI 请求装配为 core 共享协议件（envelope.c），此处仅闭
 * default_model 参。响应解析同理直接挂 provider_parse_openai_response。 */
static char *openai_build_request(const llm_request_config_t *manager)
{
    return provider_build_openai_request(manager, OPENAI_DEFAULT_MODEL);
}

static struct provider_rate_limiter *openai_rate_limiter(provider_ctx_t *ctx)
{
    return &((openai_ctx_t *)ctx)->rl;
}

static int openai_complete(provider_ctx_t *ctx, const llm_request_config_t *manager,
                           llm_response_t **out_response)
{
    return provider_driver_complete(ctx, &openai_ops, manager, out_response);
}

static int openai_complete_stream(provider_ctx_t *ctx, const llm_request_config_t *manager,
                                  llm_stream_callback_t callback, void *callback_data,
                                  llm_response_t **out_response)
{
    return provider_driver_complete_stream(ctx, &openai_ops, manager, callback, callback_data,
                                           out_response);
}

/* 五槽装配：feed_event 留空（NULL）= OpenAI 行协议流，on_chunk 机制件由
 * driver 挂 core provider_openai_on_chunk（core/adapter.c 分帧模式判定）。 */
const provider_adapter_t openai_ops = {
    .name = "openai", .tag = "OPENAI",
    .default_model = OPENAI_DEFAULT_MODEL, .default_base_url = OPENAI_DEFAULT_BASE,
    .chat_path = OPENAI_CHAT_PATH, .headers = &OPENAI_HEADERS,
    .init = openai_init, .destroy = openai_destroy,
    .rate_limiter = openai_rate_limiter,
    .complete = openai_complete, .complete_stream = openai_complete_stream,
    .build_request = openai_build_request, .parse_response = provider_parse_openai_response,
};
