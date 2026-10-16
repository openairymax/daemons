/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_base.h: Provider 域 base/context 唯一契约（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_BASE_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_BASE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct provider_rate_limiter;

/* 可重试类别（策略面）：机制件 provider_retry.c 只认"哪些 libcurl 故障可重发"，
 * 是否收窄由端点自身声明。本地慢推理是预期行为而非瞬时故障，超时重发只会
 * 空转整轮预算，故本地端点声明 CONNECT_ONLY——仅连接建立失败可重发。 */
typedef enum {
    PROVIDER_RETRY_DEFAULT = 0,      /* 全类别瞬时故障可重试（云端端点默认） */
    PROVIDER_RETRY_CONNECT_ONLY = 1, /* 仅连接建立失败可重试；超时/收发中断即终局 */
} provider_retry_class_t;

typedef struct {
    char api_key[256];
    char api_key_env[128];
    char api_base[512];
    char organization[128];
    double timeout_sec;
    int max_retries;
    provider_retry_class_t retry_class;
} provider_base_ctx_t;

typedef enum {
    PROVIDER_AUTH_NONE = 0,
    PROVIDER_AUTH_BEARER,
    PROVIDER_AUTH_X_API_KEY,
} provider_auth_kind_t;

typedef struct {
    provider_auth_kind_t auth;
    const char *const *extra;
    size_t extra_count;
} provider_header_spec_t;

void provider_base_init(provider_base_ctx_t *base_ctx, const char *provider_name,
                        const char *api_key, const char *api_base, const char *organization,
                        double timeout_sec, int max_retries, const char *default_base);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_BASE_H */
