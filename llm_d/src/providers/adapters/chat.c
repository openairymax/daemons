// SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0

/**
 * @file chat.c
 * @brief Chat Completions 线格式适配器：只实现线格式本身——Bearer 鉴权、
 * "/chat/completions" 端点路径、令牌桶限流（RPM/TPM + 429 退避）。
 *
 * 端点、默认模型、附加头一律由 model.yaml 注入（default_* 槽为 NULL），
 * 本件不持有任何厂商名与厂商默认值。请求装配/响应解析复用
 * core/envelope.c 的线格式协议件；编排归 core/adapter.c driver。
 */

#include "airy_memory.h"
#include "error.h"
#include "daemon_platform_ext.h"
#include "providers/providers_internal.h"
#include "core/rate_limit.h"
#include "svc_logger.h"

#define CHAT_PATH "/chat/completions"

/* 请求头声明：Bearer 鉴权（core 按 R-1 省略空凭据），无静态附加头。 */
static const provider_header_spec_t CHAT_HEADERS = {.auth = PROVIDER_AUTH_BEARER};

/* 运行时上下文：首字段必须为 provider_base_ctx_t（provider_base_ctx()
 * 依赖此布局）。 */
typedef struct {
    provider_base_ctx_t base;
    provider_rate_limiter_t rl;
} chat_ctx_t;

extern const provider_adapter_t chat_ops; /* shim 自引用 ops */

static provider_ctx_t *chat_init(const char *name, const char *api_key, const char *api_base,
                                 const char *organization, double timeout_sec, int max_retries)
{
    chat_ctx_t *ctx = (chat_ctx_t *)AIRY_CALLOC(1, sizeof(chat_ctx_t));
    if (!ctx) {
        SVC_LOG_ERROR("C-L02: CHAT: INIT-FAIL reason=oom STACK: chat_init");
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "null parameter");
    }

    provider_base_init(&ctx->base, name, api_key, api_base, organization, timeout_sec, max_retries,
                       NULL);

    /* 线格式不持有默认端点：未由配置注入 base_url 即无法出网，fail-closed
     * 优于静默打到某个厂商地址。 */
    if (!ctx->base.api_base[0]) {
        SVC_LOG_ERROR("C-L02: CHAT: INIT-FAIL reason=no_base_url provider=%s STACK: chat_init",
                      name ? name : "");
        AIRY_FREE(ctx);
        AIRY_ERROR_NULL(AIRY_ERR_INVALID_PARAM, "missing api_base");
    }

    provider_rl_init(&ctx->rl);

    airy_random_init();

    SVC_LOG_INFO("C-L02: CHAT: INIT api_base=%s timeout=%.1fs retries=%d has_api_key=%d RPM=%d",
                 ctx->base.api_base, ctx->base.timeout_sec, ctx->base.max_retries,
                 ctx->base.api_key[0] ? 1 : 0, PROVIDER_RL_DEFAULT_RPM);

    return (provider_ctx_t *)ctx;
}

static void chat_destroy(provider_ctx_t *ctx_ptr)
{
    if (!ctx_ptr)
        return;
    chat_ctx_t *ctx = (chat_ctx_t *)ctx_ptr;
    SVC_LOG_INFO("C-L02: CHAT: DESTROY ctx=%p", (void *)ctx_ptr);
    provider_rl_destroy(&ctx->rl);
    AIRY_FREE(ctx_ptr);
}

static char *chat_build_request(const llm_request_config_t *manager)
{
    return provider_build_chat_req(manager, NULL, NULL);
}

static struct provider_rate_limiter *chat_rate_limiter(provider_ctx_t *ctx)
{
    return &((chat_ctx_t *)ctx)->rl;
}

static int chat_complete(provider_ctx_t *ctx, const llm_request_config_t *manager,
                         llm_response_t **out_response)
{
    return provider_driver_complete(ctx, &chat_ops, manager, out_response);
}

static int chat_complete_stream(provider_ctx_t *ctx, const llm_request_config_t *manager,
                                llm_stream_callback_t callback, void *callback_data,
                                llm_response_t **out_response)
{
    return provider_driver_complete_stream(ctx, &chat_ops, manager, callback, callback_data,
                                           out_response);
}

/* 五槽装配：feed_event 留空（NULL）= 行协议流，on_chunk 机制件由 driver
 * 挂 core provider_stream_on_chunk（core/adapter.c 分帧模式判定）。 */
const provider_adapter_t chat_ops = {
    .name = "chat", .tag = "CHAT",
    .default_model = NULL, .default_base_url = NULL,
    .chat_path = CHAT_PATH, .headers = &CHAT_HEADERS,
    .init = chat_init, .destroy = chat_destroy,
    .rate_limiter = chat_rate_limiter,
    .complete = chat_complete, .complete_stream = chat_complete_stream,
    .build_request = chat_build_request, .parse_response = provider_parse_chat_resp,
};
