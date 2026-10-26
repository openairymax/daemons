/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */
/* provider_base.h: Provider 域 base/context 唯一契约（SSoT，B16-S3） */

#ifndef LLM_D_PROVIDERS_CORE_PROVIDER_BASE_H
#define LLM_D_PROVIDERS_CORE_PROVIDER_BASE_H

#include "provider_retry_class.h"
#include "llm_service_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct provider_rate_limiter;

typedef struct {
    char api_key[256];
    char api_key_env[128];
    char api_base[512];
    char organization[128];
    /* 端点附加头（如协议版本头），由 model.yaml extra_headers 注入，多条以 ';' 或换行分隔。 */
    char extra_headers[512];
    double timeout_sec;
    int max_retries;
    provider_retry_class_t retry_class;
    /* C11 能力位：registry 从 provider_config_t 盖入，driver 装配请求时
     * 盖印到 llm_request_config_t.caps，envelope 按位条件注入可选字段。 */
    local_caps_t caps;
} provider_base_ctx_t;

void provider_base_init(provider_base_ctx_t *base_ctx, const char *provider_name,
                        const char *api_key, const char *api_base, const char *organization,
                        double timeout_sec, int max_retries, const char *default_base);

#ifdef __cplusplus
}
#endif

#endif /* LLM_D_PROVIDERS_CORE_PROVIDER_BASE_H */
